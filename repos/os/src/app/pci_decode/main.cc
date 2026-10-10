/*
 * \brief  PCI configuration space decoder
 * \author Stefan Kalkowski
 * \date   2021-12-12
 */

/*
 * Copyright (C) 2021 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#include <base/attached_io_mem_dataspace.h>
#include <base/attached_rom_dataspace.h>
#include <base/component.h>
#include <base/env.h>
#include <base/heap.h>
#include <os/reporter.h>
#include <base/attached_io_mem_dataspace.h>

#include <irq.h>
#include <rmrr.h>
#include <drhd.h>
#include <ioapic.h>
#include <pci/config.h>

using namespace Genode;
using namespace Pci;


struct Main
{
	Env                   &env;
	Heap                   heap            { env.ram(), env.rm()       };
	Attached_rom_dataspace platform_info   { env, "platform_info"      };
	Attached_rom_dataspace config         { env, "config"             };
	Expanding_reporter     pci_reporter    { env, "devices", "devices", { 32*1024 } };
	Registry<Bridge>       bridge_registry {}; /* contains host bridges */

	bool apic_capable { false };
	bool msi_capable  { false };

	/*
	 * PCIe Precision Time Measurement.
	 *
	 * The capability is REPORTED for every device that has one, which is a
	 * pure read and carries no risk. Enabling is opt-in per device, because
	 * a requester can only be enabled by also enabling the responder above
	 * it -- which is a root port, a device shared with everything else
	 * behind it. On the machine this was written for, the port carrying the
	 * only console sits behind a *different* root port, and that separation
	 * is worth nothing if the enable is applied wherever the hardware
	 * happens to allow it. So:
	 *
	 *   <config>
	 *     <ptm device="57:00.0"/>              <!-- report what would happen -->
	 *     <ptm device="57:00.0" write="yes"/>  <!-- actually enable it -->
	 *   </config>
	 *
	 * Naming an endpoint implies its upstream responder: the walk is
	 * top-down, so a root port is reached before anything behind it, and it
	 * is enabled if any requested device lies within its bus range.
	 */
	struct Ptm_request
	{
		bool requested { false };
		bool write     { false };
		bool verify    { false };
	};

	/*
	 * Prove the write reaches the device before trusting a readback.
	 *
	 * On the machine this was developed against, firmware enables PTM during
	 * POST, so the control register ALREADY holds the value we would write.
	 * Writing it and reading it back therefore demonstrates nothing: a write
	 * that never left the component would produce an identical log. So with
	 * verify="yes" the register is first cleared and read back, which fails
	 * visibly if the write is not reaching hardware, and only then is the
	 * intended value written.
	 */
	bool ptm_write_reaches_device(Pci::Bdf const &, Config &);

	Ptm_request ptm_request_for_device(Pci::Bdf const &) const;
	Ptm_request ptm_request_behind(Pci::bus_t from, Pci::bus_t to) const;

	void handle_ptm_bridge(Pci::Bdf const &, Config &,
	                       Bridge const &parent, Bridge &self);
	void handle_ptm_device(Pci::Bdf const &, Config &);
	void report_ptm(Config &, Generator &);

	struct Intel_opregion
	{
		addr_t start { 0 };
		size_t size  { 0 };

		bool valid() { return size != 0; }
	} intel_opregion { };

	List_model<Irq_routing>  irq_routing_list  {};
	List_model<Irq_override> irq_override_list {};
	List_model<Rmrr>         reserved_memory_list {};
	List_model<Drhd>         drhd_list {};
	List_model<Ioapic>       ioapic_list {};

	Constructible<Attached_io_mem_dataspace> pci_config_ds {};

	bus_t parse_pci_function(Bdf, Config &,
	                         addr_t cfg_phys_base,
	                         Generator &);
	bus_t parse_pci_bus(bus_t bus, Byte_range_ptr const &, addr_t phys_base,
	                    Generator &);

	void parse_irq_override_rules(Node const &);
	void parse_pci_config_spaces (Node const &, Generator &);
	void parse_acpi_device_info  (Node const &, Generator &);
	void parse_tpm2_table        (Node const &, Generator &);

	Intel_opregion parse_intel_opregion(Pci::Config const &);

	template <typename FN>
	void for_bridge(Pci::bus_t bus, FN const &fn)
	{
		bridge_registry.for_each([&] (Bridge &b) {
			if (b.behind(bus)) b.find_bridge(bus, fn); });
	}

	Main(Env &env);
};


/*
 * If PCI devices happen to miss complete configuration after boot, i.e., have
 * a zero base address, we report a fixed address for known devices.
 * platform in turn, will setup the address from the report when enabling
 * the device.
 *
 * The issue was discovered with Intel LPSS devices in Fujitsu notebooks.
 *
 * XXX static fixup list should be replaced by dynamic mapping of BAR
 */
static uint64_t fixup_bar_base_address(Config const &cfg,
                                       Bdf bdf, unsigned bar, uint64_t addr, uint64_t size)
{
	auto base_address = addr;

	using C0 = Config_type0;
	C0 const cfg0(cfg.range());

	auto const vendor_id = cfg0.read<C0::Vendor>();
	auto const device_id = cfg0.read<C0::Device>();

	auto const subsystem_vendor_id = cfg0.read<C0::Subsystem_vendor>();

	/* Intel LPSS (I2C) devices - values taken from Linux boot */
	if (vendor_id == 0x8086 /* Intel */) {

		if (subsystem_vendor_id == 0x1e26 /* Fujitsu */) {
			if (bdf == Bdf { 0, 0x15, 0 } && bar == 0) base_address = 0x4017000000;
			if (bdf == Bdf { 0, 0x15, 1 } && bar == 0) base_address = 0x4017001000;
			if (bdf == Bdf { 0, 0x15, 2 } && bar == 0) base_address = 0x4017001000;
			if (bdf == Bdf { 0, 0x15, 3 } && bar == 0) base_address = 0x4017002000;
		}

		if (subsystem_vendor_id == 0x103c /* HP */) {
			if (bdf == Bdf { 0, 0x15, 0 } && bar == 0) base_address = 0x501931d000; /* I2C */
			if (bdf == Bdf { 0, 0x1e, 2 } && bar == 0) base_address = 0x5019320000; /* SPI */
		}

		/* StarLite Alder Lake-N */
		if (subsystem_vendor_id == 0x8086 && (device_id == 0x54e8 || device_id == 0x54ea)) {
			if (bdf == Bdf { 0, 0x15, 0 } && bar == 0) base_address = 0x80626000;
			if (bdf == Bdf { 0, 0x15, 2 } && bar == 0) base_address = 0x80627000;
		}

		/* Framework 13 Tiger/Alder/Raptor Lake */
		if (subsystem_vendor_id == 0xf111 && (device_id == 0x51eb || device_id == 0xa0eb)) {
			if (bdf == Bdf { 0, 0x15, 3 } && bar == 0) base_address = 0x4017002000;
		}

		/* Framework 13 Meteor Lake */
		if (subsystem_vendor_id == 0xf111 && (device_id == 0x7e50 || device_id == 0x7e51)) {
			if (bdf == Bdf { 0, 0x19, 0 } && bar == 0) base_address = 0x60192fc000;
			if (bdf == Bdf { 0, 0x19, 1 } && bar == 0) base_address = 0x60192fd000;
		}
	}

	if (addr != base_address)
		log(bdf, " remap MEM BAR", bar, " ", Hex_range(addr, (size_t)size), " to ", Hex(base_address));

	return base_address;
}


/*
 * Similar to 'fixup_bar_base_address', we report a fixed IRQ number for
 * known devices.
 */
static irq_line_t fixup_irq_number(Config const &cfg,
                                   Bdf bdf, irq_line_t irq)
{
	irq_line_t remapped_irq = irq;

	using C0 = Config_type0;
	C0 const cfg0(cfg.range());

	auto const vendor_id = cfg0.read<C0::Vendor>();
	auto const device_id = cfg0.read<C0::Device>();

	auto const subsystem_vendor_id = cfg0.read<C0::Subsystem_vendor>();

	/* Intel LPSS (I2C) devices - values taken from Linux boot */
	if (vendor_id == 0x8086 /* Intel */) {

		/* StarLite Alder Lake-N */
		if (subsystem_vendor_id == 0x8086 && (device_id == 0x54e8 || device_id == 0x54ea)) {
			if (bdf == Bdf { 0, 0x15, 0 } && irq == 255) remapped_irq = 37;
			if (bdf == Bdf { 0, 0x15, 2 } && irq == 255) remapped_irq = 39;
		}
	}

	if (irq != remapped_irq)
		log(bdf, " remap IRQ ", irq, " to ", remapped_irq);

	return remapped_irq;
}


/*
 * The bus and function parsers return either the current bus number or the
 * subordinate bus number (highest bus number of all of the busses that can be
 * reached downstream of a bridge).
 */

/*
 * The bus number out of a "bb:dd.f" string, which is how Pci::Bdf renders
 * itself and therefore how a device is named in configuration. Hex, so
 * ascii_to() would read "57" as fifty-seven rather than 0x57.
 */
static bool parse_bdf_bus(Genode::String<16> const &s, bus_t &out)
{
	char const *p = s.string();
	unsigned v = 0, n = 0;

	for (; *p && *p != ':'; p++, n++) {
		unsigned d;
		if      (*p >= '0' && *p <= '9') d = (unsigned)(*p - '0');
		else if (*p >= 'a' && *p <= 'f') d = (unsigned)(*p - 'a') + 10;
		else if (*p >= 'A' && *p <= 'F') d = (unsigned)(*p - 'A') + 10;
		else return false;
		v = v*16 + d;
	}

	if (!n || *p != ':' || v > 0xff)
		return false;

	out = (bus_t)v;
	return true;
}


Main::Ptm_request Main::ptm_request_for_device(Bdf const &bdf) const
{
	Ptm_request result { };
	String<16> const want = Bdf::string(bdf);

	config.node().for_each_sub_node("ptm", [&] (Node const &node) {
		String<16> const got = node.attribute_value("device", String<16>());
		if (strcmp(got.string(), want.string()))
			return;
		result.requested = true;
		if (node.attribute_value("write", false))
			result.write = true;
		if (node.attribute_value("verify", false))
			result.verify = true;
	});

	return result;
}


Main::Ptm_request Main::ptm_request_behind(bus_t from, bus_t to) const
{
	Ptm_request result { };

	config.node().for_each_sub_node("ptm", [&] (Node const &node) {
		String<16> const got = node.attribute_value("device", String<16>());
		bus_t bus = 0;
		if (!parse_bdf_bus(got, bus)) {
			warning("ptm: cannot read a bus number from device=\"", got, "\"");
			return;
		}
		if (bus < from || bus > to)
			return;
		result.requested = true;
		if (node.attribute_value("write", false))
			result.write = true;
		if (node.attribute_value("verify", false))
			result.verify = true;
	});

	return result;
}


/*
 * Effective granularity along a path. The encoding is nanoseconds EXCEPT
 * that 0 means "not indicated", so an unknown value anywhere makes the
 * result unknown rather than making it the minimum.
 */
static Genode::uint8_t ptm_path_granularity(Genode::uint8_t upstream,
                                            Genode::uint8_t local)
{
	if (!upstream || !local)
		return 0;

	return upstream > local ? upstream : local;
}


bool Main::ptm_write_reaches_device(Bdf const &bdf, Config &cfg)
{
	using Ptm = Config::Ptm_capability;

	auto const before = cfg.ptm_cap->read<Ptm::Control>();

	cfg.ptm_cap->write<Ptm::Control>(0);
	auto const cleared = cfg.ptm_cap->read<Ptm::Control>();

	bool const reaches = (cleared == 0);

	if (reaches)
		log("ptm ", bdf, " verify: cleared ",
		    Hex(before, Hex::PREFIX, Hex::PAD), " and read back 0x0, so the "
		    "write reaches the device");
	else
		error("ptm ", bdf, " verify: wrote 0x0 but read back ",
		      Hex(cleared, Hex::PREFIX, Hex::PAD),
		      " -- the write is NOT reaching the device, so any readback "
		      "below proves nothing");

	return reaches;
}


void Main::handle_ptm_bridge(Bdf const &bdf, Config &cfg,
                             Bridge const &parent, Bridge &self)
{
	if (!cfg.ptm_cap.constructed())
		return;

	using Ptm = Config::Ptm_capability;

	auto const cap  = cfg.ptm_cap->read<Ptm::Capability>();
	auto const ctrl = cfg.ptm_cap->read<Ptm::Control>();

	bool    const root      = Ptm::Capability::Root::get(cap);
	bool    const responder = Ptm::Capability::Responder::get(cap);
	uint8_t const gran      = (uint8_t)Ptm::Capability::Granularity::get(cap);

	Ptm_request const req = ptm_request_behind(self.from, self.to);

	/*
	 * Record the granularity even when nothing is enabled, so that a dry run
	 * can still report what a requester below would have been given.
	 */
	self.ptm_granularity = root ? gran
	                            : ptm_path_granularity(parent.ptm_granularity,
	                                                   gran);

	if (!req.requested)
		return;

	if (!root && !(responder && parent.ptm_enabled)) {
		warning("ptm ", bdf, ": requested but it is neither a PTM root nor a "
		        "responder behind an enabled one");
		return;
	}

	/*
	 * Enable, plus Root Select on a root. The control register's granularity
	 * field is left at zero on a root and written on a responder -- which is
	 * the configuration measured as working on this hardware under Linux:
	 * root ctrl 0x3 with the field zero, requester ctrl 0x401 with it set.
	 */
	Ptm::Control::access_t v = 0;
	Ptm::Control::Enable::set(v, 1);
	if (root)
		Ptm::Control::Root_select::set(v, 1);
	else
		Ptm::Control::Granularity::set(v, self.ptm_granularity);

	log("ptm ", bdf, " ", root ? "root" : "responder",
	    " cap=", Hex(cap, Hex::PREFIX, Hex::PAD),
	    " gran=", gran,
	    " ctrl=", Hex(ctrl, Hex::PREFIX, Hex::PAD),
	    req.write ? " -> write " : " -> would write ",
	    Hex(v, Hex::PREFIX, Hex::PAD));

	if (!req.write)
		return;

	if (req.verify)
		ptm_write_reaches_device(bdf, cfg);

	cfg.ptm_cap->write<Ptm::Control>(v);
	self.ptm_enabled = true;

	log("ptm ", bdf, " readback ctrl=",
	    Hex(cfg.ptm_cap->read<Ptm::Control>(), Hex::PREFIX, Hex::PAD));
}


void Main::handle_ptm_device(Bdf const &bdf, Config &cfg)
{
	if (!cfg.ptm_cap.constructed())
		return;

	using Ptm = Config::Ptm_capability;

	auto const cap  = cfg.ptm_cap->read<Ptm::Capability>();
	auto const ctrl = cfg.ptm_cap->read<Ptm::Control>();

	bool    const requester = Ptm::Capability::Requester::get(cap);
	uint8_t const gran      = (uint8_t)Ptm::Capability::Granularity::get(cap);

	Ptm_request const req = ptm_request_for_device(bdf);
	if (!req.requested)
		return;

	if (!requester) {
		warning("ptm ", bdf, ": requested but not requester-capable");
		return;
	}

	bool    upstream_enabled = false;
	uint8_t upstream_gran    = 0;
	for_bridge(bdf.bus, [&] (Bridge &b) {
		upstream_enabled = b.ptm_enabled;
		upstream_gran    = b.ptm_granularity;
	});

	Ptm::Control::access_t v = 0;
	Ptm::Control::Enable::set(v, 1);
	Ptm::Control::Granularity::set(v, ptm_path_granularity(upstream_gran,
	                                                       gran));

	log("ptm ", bdf, " requester",
	    " cap=", Hex(cap, Hex::PREFIX, Hex::PAD),
	    " gran=", gran, " upstream_gran=", upstream_gran,
	    " ctrl=", Hex(ctrl, Hex::PREFIX, Hex::PAD),
	    (req.write && upstream_enabled) ? " -> write " : " -> would write ",
	    Hex(v, Hex::PREFIX, Hex::PAD));

	if (!upstream_enabled) {
		log("ptm ", bdf, ": upstream responder is not enabled, so this "
		    "requester is left alone");
		return;
	}

	if (!req.write)
		return;

	if (req.verify)
		ptm_write_reaches_device(bdf, cfg);

	cfg.ptm_cap->write<Ptm::Control>(v);

	log("ptm ", bdf, " readback ctrl=",
	    Hex(cfg.ptm_cap->read<Ptm::Control>(), Hex::PREFIX, Hex::PAD));
}


void Main::report_ptm(Config &cfg, Generator &g)
{
	if (!cfg.ptm_cap.constructed())
		return;

	using Ptm = Config::Ptm_capability;

	auto const cap  = cfg.ptm_cap->read<Ptm::Capability>();
	auto const ctrl = cfg.ptm_cap->read<Ptm::Control>();

	g.node("ptm", [&] {
		if (Ptm::Capability::Requester::get(cap)) g.attribute("requester", true);
		if (Ptm::Capability::Responder::get(cap)) g.attribute("responder", true);
		if (Ptm::Capability::Root::get(cap))      g.attribute("root",      true);

		g.attribute("granularity",
		            (unsigned)Ptm::Capability::Granularity::get(cap));

		if (Ptm::Control::Enable::get(ctrl))      g.attribute("enabled", true);
		if (Ptm::Control::Root_select::get(ctrl)) g.attribute("root_selected", true);

		g.attribute("effective_granularity",
		            (unsigned)Ptm::Control::Granularity::get(ctrl));
	});
}


bus_t Main::parse_pci_function(Bdf        bdf,
                               Config    &cfg,
                               addr_t     cfg_phys_base,
                               Generator &g)
{
	cfg.scan();

	bus_t subordinate_bus = bdf.bus;

	/* check for bridges */
	if (cfg.read<Config::Header_type::Type>()) {
		for_bridge(bdf.bus, [&] (Bridge &parent) {
			Config_type1 bcfg(cfg.range());
			Bridge &self = *new (heap) Bridge(parent.sub_bridges, bdf,
			                                  bcfg.secondary_bus_number(),
			                                  bcfg.subordinate_bus_number());

			subordinate_bus = bcfg.subordinate_bus_number();

			handle_ptm_bridge(bdf, cfg, parent, self);

			/* enable I/O spaces and DMA in bridges if not done already */
			using Command = Pci::Config::Command;
			Command::access_t command = bcfg.read<Command>();
			if (Command::Io_space_enable::get(command)     == 0 ||
			    Command::Memory_space_enable::get(command) == 0 ||
			    Command::Bus_master_enable::get(command)   == 0) {
				Command::Io_space_enable::set(command, 1);
				Command::Memory_space_enable::set(command, 1);
				Command::Bus_master_enable::set(command, 1);
				bcfg.write<Command>(command);
			}
		});
	}

	/* a requester is an endpoint; a responder was handled in the branch above */
	if (!cfg.read<Config::Header_type::Type>())
		handle_ptm_device(bdf, cfg);

	bool      msi     = msi_capable && cfg.msi_cap.constructed();
	bool      msi_x   = msi_capable && cfg.msi_x_cap.constructed();
	irq_pin_t irq_pin = cfg.read<Config::Irq_pin>();

	enum { VENDOR_INTEL = 0x8086, CLASS_DISPLAY = 3 };

	bool intel_graphics_card =
		(bdf == Bdf(0,2,0)) &&
		(cfg.read<Config::Vendor>() == VENDOR_INTEL) &&
		(cfg.read<Config::Base_class_code>() == CLASS_DISPLAY);

	/* disable MSI/MSI-X by default */
	if (msi) cfg.msi_cap->write<Pci::Config::Msi_capability::Control::Enable>(0);
	if (msi_x) cfg.msi_x_cap->write<Pci::Config::Msi_x_capability::Control::Enable>(0);

	{
		/* Apply GSI/MSI/MSI-X quirks based on vendor/device */
		auto const vendor_id = cfg.read<Config::Vendor>();
		auto const device_id = cfg.read<Config::Device>();

		/*
		 * Force use of GSI on given ath9k device as using MSI
		 * does not work.
		 */
		if (vendor_id == 0x168c || device_id == 0x0034)
			msi = msi_x = false;
	}

	if (intel_graphics_card) intel_opregion = parse_intel_opregion(cfg);

	/* XXX we might need to skip PCI-discoverable IOAPIC and IOMMU devices */

	g.node("device", [&]
	{
		auto string = [&] (uint64_t v) { return String<16>(Hex(v)); };

		g.attribute("name", Bdf::string(bdf));
		g.attribute("type", "pci");

		/* we limit MSI vectors to just one, due to continuity constraints  */
		if (msi)   g.attribute("msi", 1UL);
		if (msi_x) g.attribute("msi_x", cfg.msi_x_cap->vectors());

		g.node("pci-config", [&]
		{
			using C  = Config;
			using C0 = Config_type0;
			using C1 = Config_type1;
			using Cc = Config::Class_code_rev_id;

			g.attribute("address",       string(cfg_phys_base));
			g.attribute("bus",           string(bdf.bus));
			g.attribute("device",        string(bdf.dev));
			g.attribute("function",      string(bdf.fn));
			g.attribute("vendor_id",     string(cfg.read<C::Vendor>()));
			g.attribute("device_id",     string(cfg.read<C::Device>()));
			g.attribute("class",         string(cfg.read<Cc::Class_code>()));
			g.attribute("revision",      string(cfg.read<Cc::Revision>()));
			g.attribute("bridge",        cfg.bridge() ? "yes" : "no");

			if (cfg.bridge()) {
				C1 cfg1(cfg.range());
				g.attribute("io_base_limit",
				            string(cfg1.read<C1::Io_base_limit>()));
				g.attribute("memory_base",
				            string(cfg1.read<C1::Memory_base>()));
				g.attribute("memory_limit",
				            string(cfg1.read<C1::Memory_limit>()));
				g.attribute("prefetch_memory_base",
				            string(cfg1.read<C1::Prefetchable_memory_base>()));
				g.attribute("prefetch_memory_base_upper",
				            string(cfg1.read<C1::Prefetchable_memory_base_upper>()));
				g.attribute("prefetch_memory_limit_upper",
				            string(cfg1.read<C1::Prefetchable_memory_limit_upper>()));
				g.attribute("io_base_limit_upper",
				            string(cfg1.read<C1::Io_base_limit_upper>()));
				g.attribute("expansion_rom_base",
				            string(cfg1.read<C1::Expansion_rom_base_addr>()));
				g.attribute("bridge_control",
				            string(cfg1.read<C1::Bridge_control>()));
			} else {
				C0 cfg0(cfg.range());
				g.attribute("sub_vendor_id",
				            string(cfg0.read<C0::Subsystem_vendor>()));
				g.attribute("sub_device_id",
				            string(cfg0.read<C0::Subsystem_device>()));
			}
		});

		report_ptm(cfg, g);

		cfg.for_each_bar([&] (uint64_t addr, uint64_t size,
		                      unsigned bar, bool pf)
		{
			addr = fixup_bar_base_address(cfg, bdf, bar, addr, size);
			if (!addr)
				warning(bdf, " MEM BAR", bar, " ", Hex_range(addr, (size_t)size),
				        " has invalid base address - consider pci-fixup in parse_pci_function()");
			g.node("io_mem", [&]
			{
				g.attribute("pci_bar", bar);
				g.attribute("address", string(addr));
				g.attribute("size",    string(size));
				if (!pf) return;

				g.attribute("prefetchable", true);

				/*
				 * According to the PCI spec: A PCI Express Function requesting
				 * Memory Space through a BAR must set the BAR's Prefetchable
				 * bit unless the range contains locations with read side
				 * effects or locations in which the Function does
				 * not tolerate write merging (a.k.a. write-combined access).
				 *
				 * Some devices (e.g., Intel Meteor Lake and later IGD) seem to
				 * set the prefetchable bit for BARs containing MMIO registers
				 * for unknown reasons. For those we report wc=false below to.
				 * prohibit write combining for the BAR.
				 */
				bool wc = true;

				/*
				 * Intel integrated graphics device (IGD) BAR 0 contains MMIO
				 * registers and the GTT. While the latter may be accessed
				 * write-combined, MMIO registers must be mapped uncached.
				 */
				if (bar == 0 && intel_graphics_card)
					wc = false;

				g.attribute("wc", wc);
			});
		}, [&] (uint64_t addr, uint64_t size, unsigned bar) {
			g.node("io_port_range", [&]
			{
				g.attribute("pci_bar", bar);
				g.attribute("address", string(addr));

				/* on x86 I/O ports can be in range 0-64KB only */
				g.attribute("size", string(size & 0xffff));
			});
		});

		if (intel_graphics_card && intel_opregion.valid())
			g.node("io_mem", [&]
			{
				g.attribute("pci_bar", 6); /* take +1 max PCI BAR */
				g.attribute("address", String<20>(Hex(intel_opregion.start)));
				g.attribute("size",    intel_opregion.size);
			});

		/*
		 * Only generate <irq> nodes if:
		 *
		 * - An IRQ pin from 1-4 (INTA-D) specifies legacy IRQ or GSI can be
		 *   used, zero means no IRQ defined.
		 */
		if (irq_pin != 0)
			g.node("irq", [&]
			{
				irq_line_t irq = cfg.read<Config::Irq_line>();

				for_bridge(bdf.bus, [&] (Bridge &b) {
					irq_routing_list.for_each([&] (Irq_routing &ir) {
						ir.route(b, bdf.dev, irq_pin-1, irq); });
				});

				irq_override_list.for_each([&] (Irq_override &io) {
					io.generate(g, irq); });

				irq = fixup_irq_number(cfg, bdf, irq);

				g.attribute("number", irq);
			});

		reserved_memory_list.for_each([&] (Rmrr &rmrr) {
			if (rmrr.bdf == bdf)
				g.node("reserved_memory", [&]
				{
					g.attribute("address", rmrr.addr);
					g.attribute("size",    rmrr.size);
				});
		});

		/* XXX We currently only support unsegmented platforms with a single
		 *     pci config space. Yet as soon as we do support those, we
		 *     must assign the DMA-remapping hardware unit to the different pci
		 *     segments resp. their devices.
		 */

		bool drhd_device_found = false;
		drhd_list.for_each([&] (Drhd const &drhd) {
			if (drhd_device_found) return;

			bool device_match = false;
			drhd.devices.for_each([&] (Drhd::Device const &device) {
				if (device.bdf == bdf)
					device_match = true;
			});

			if (device_match) {
				drhd_device_found = true;
				g.node("io_mmu", [&] { g.attribute("name", drhd.name()); });
			}
		});

		if (!drhd_device_found) {
			drhd_list.for_each([&] (Drhd const &drhd) {
				if (drhd.scope == Drhd::Scope::INCLUDE_PCI_ALL)
					g.node("io_mmu", [&] { g.attribute("name", drhd.name()); });
			});
		}
	});

	return subordinate_bus;
}


bus_t Main::parse_pci_bus(bus_t                 bus,
                          Byte_range_ptr const &range,
                          addr_t                phys_base,
                          Generator            &g)
{
	bus_t max_subordinate_bus = bus;

	auto per_function = [&] (Byte_range_ptr const &config_range, addr_t config_phys_base,
	                         dev_t dev, func_t fn) {
		Config cfg(config_range);
		if (!cfg.valid())
			return true;

		bus_t const subordinate_bus =
			parse_pci_function({(bus_t)bus, dev, fn}, cfg,
			                   config_phys_base, g);

		max_subordinate_bus = max(max_subordinate_bus, subordinate_bus);

		return !(fn == 0 && !cfg.read<Config::Header_type::Multi_function>());
	};

	for (dev_t dev = 0; dev < DEVICES_PER_BUS_MAX; dev++) {
		for (func_t fn = 0; fn < FUNCTION_PER_DEVICE_MAX; fn++) {
			unsigned factor = dev * FUNCTION_PER_DEVICE_MAX + fn;
			off_t config_offset = factor * FUNCTION_CONFIG_SPACE_SIZE;
			Byte_range_ptr config_range { range.start + config_offset, range.num_bytes - config_offset };
			addr_t config_phys_base = phys_base + config_offset;
			if (!per_function(config_range, config_phys_base, dev, fn))
				break;
		}
	}

	return max_subordinate_bus;
}


static void parse_acpica_info(Node const &node,
                              Main::Intel_opregion intel_opregion,
                              Generator &g)
{
	g.node("device", [&] {
		g.attribute("name", "acpi");
		g.attribute("type", "acpi");

		if (intel_opregion.valid())
			g.node("io_mem", [&]
			{
				g.attribute("address", String<20>(Hex(intel_opregion.start)));
				g.attribute("size",    intel_opregion.size);
			});

		node.with_optional_sub_node("sci_int", [&] (Node const &node) {
			g.node("irq", [&] {
				g.attribute("number", node.attribute_value("irq", 0xff));
			});
		});
	});
}

/*
 * Parse the TPM2 ACPI table and report the device if available.
 *
 * See the following document for further information:
 * https://trustedcomputinggroup.org/wp-content/uploads/TCG_ACPIGeneralSpec_v1p3_r8_pub.pdf
 */
void Main::parse_tpm2_table(Node const &node, Generator &g)
{
	enum {
		TPM2_TABLE_CRB_ADDRESS_OFFSET = 40,
		TPM2_TABLE_CRB_ADDRESS_MASK = (~0xfff),
		TPM2_TABLE_START_METHOD_OFFSET = 48,
		TPM2_TABLE_START_METHOD_MEMORY_MAPPED = 6,
		TPM2_TABLE_START_METHOD_CRB = 7,
		TPM2_TABLE_MIN_SIZE = 52UL,
		TPM2_DEVICE_IO_MEM_SIZE = 0x1000U,
	};

	addr_t const addr = node.attribute_value("addr", 0UL);
	size_t const size = node.attribute_value("size", 0UL);

	if ((addr < 1UL) || (size < TPM2_TABLE_MIN_SIZE)) {
		error("TPM2 table info invalid");
		return;
	}

	Attached_io_mem_dataspace io_mem { env, addr, size };
	char* ptr = io_mem.local_addr<char>();

	if (memcmp(ptr, "TPM2", 4) != 0) {
		error("TPM2 table parse error");
		return;
	}

	auto start_method = *reinterpret_cast<uint32_t*>(ptr + TPM2_TABLE_START_METHOD_OFFSET);

	addr_t tpm_address = 0;

	String<16> type { };

	switch (start_method) {
	case TPM2_TABLE_START_METHOD_CRB:
		tpm_address = *reinterpret_cast<addr_t*>(ptr + TPM2_TABLE_CRB_ADDRESS_OFFSET) & TPM2_TABLE_CRB_ADDRESS_MASK;
		type = "tpm2_crb";
		break;
	case TPM2_TABLE_START_METHOD_MEMORY_MAPPED:
		/* Fix defined address, see specification
		 * TCG PC Client Specific TPM Interface Specification (TIS)
		 */
		tpm_address = 0xfed4'0000;
		type = "tpm2_fifo";
		break;
	default:
		warning("Unsupported TPM2 device found ", start_method);
		return;
	}

	g.node("device", [&]
	{
		g.attribute("name", type);
		g.node("io_mem", [&] {
			g.attribute("address", tpm_address);
			g.attribute("size", TPM2_DEVICE_IO_MEM_SIZE);
		});
	});
}

/*
 * By now, we do not have the necessary information about non-PCI devices
 * available from the ACPI tables, therefore we hard-code typical devices
 * we assume to be found in this function. In the future, this function
 * shall interpret ACPI tables information.
 */
void Main::parse_acpi_device_info(Node const &node, Generator &g)
{
	using Table_name = String<5>;

	node.for_each_sub_node("table", [&] (Node const &table) {
		Table_name name = table.attribute_value("name", Table_name());
		/* Trusted Platform Module table */
		if (name == "TPM2") {
			parse_tpm2_table(table, g);
		}

		/* Non-HD-Audio-Link table */
		if (name == "NHLT") {
			addr_t const addr = table.attribute_value("addr", 0UL);
			size_t const size = table.attribute_value("size", 0UL);

			if (!addr || !size) {
				warning("NHLT invalid");
				return;
			}

			g.node("device", [&]
			{
				g.attribute("name", "NHLT");
				g.attribute("type", "acpi-table");
				g.node("io_mem", [&] {
					g.attribute("address", String<20>(Hex(addr)));
					g.attribute("size", size);
				});
			});
		}
	});

	/*
	 * PS/2 device
	 */
	g.node("device", [&]
	{
		g.attribute("name", "ps2");
		g.node("irq", [&] { g.attribute("number", 1U); });
		g.node("irq", [&] { g.attribute("number", 12U); });
		g.node("io_port_range", [&]
		{
			g.attribute("address", "0x60");
			g.attribute("size", 1U);
		});
		g.node("io_port_range", [&]
		{
			g.attribute("address", "0x64");
			g.attribute("size", 1U);
		});
	});

	/*
	 * PIT device
	 */
	g.node("device", [&]
	{
		g.attribute("name", "pit");
		g.node("irq", [&] { g.attribute("number", 0U); });
		g.node("io_port_range", [&]
		{
			g.attribute("address", "0x40");
			g.attribute("size", 4U);
		});
	});

	/*
	 * ACPI device (if applicable)
	 */
	if (node.has_sub_node("sci_int"))
		parse_acpica_info(node, intel_opregion, g);

	/*
	 * IOAPIC devices
	 */
	bool intr_remap = false;
	node.with_optional_sub_node("dmar", [&] (Node const &node) {
		intr_remap = node.attribute_value("intr_remap", intr_remap); });

	ioapic_list.for_each([&] (Ioapic const &ioapic) {
		g.node("device", [&]
		{
			g.attribute("name", ioapic.name());
			g.attribute("type", "ioapic");
			g.node("io_mem", [&]
			{
				g.attribute("address", String<20>(Hex(ioapic.addr)));
				g.attribute("size",    "0x1000");
			});

			/* find corresponding drhd and add <io_mmu/> node and Routing_id property */
			drhd_list.for_each([&] (Drhd const &drhd) {
				drhd.devices.for_each([&] (Drhd::Device const &device) {
					if (device.type == Drhd::Device::IOAPIC && device.id == ioapic.id) {
						g.node("io_mmu", [&] { g.attribute("name", drhd.name()); });
						g.node("property", [&]
						{
							g.attribute("name", "routing_id");
							g.attribute("value", String<10>(Hex(Pci::Bdf::rid(device.bdf))));
						});
					}
				});
			});

			g.node("property", [&]
			{
				g.attribute("name",  "irq_start");
				g.attribute("value", ioapic.base_irq);
			});

			if (!intr_remap)
				return;

			g.node("property", [&]
			{
				g.attribute("name",  "remapping");
				g.attribute("value", "yes");
			});
		});
	});

	/* Intel DMA-remapping hardware units */
	drhd_list.for_each([&] (Drhd const &drhd) {
		g.node("device", [&]
		{
			g.attribute("name", drhd.name());
			g.attribute("type", "intel_iommu");
			g.attribute("msi", 1UL);
			g.node("io_mem", [&]
			{
				g.attribute("address", String<20>(Hex(drhd.addr)));
				g.attribute("size",    String<20>(Hex(drhd.size)));
			});
		});
	});

	/*
	 * Intel Tigerlake/Alderlake PCH Pinctrl/GPIO
	 */
	g.node("device", [&]
	{
		g.attribute("name", "INT34C5");
		g.attribute("type", "acpi");
		g.node("irq", [&]
		{
			g.attribute("number", 14U);
			g.attribute("mode", "level");
			g.attribute("polarity", "low");
		});
		g.node("io_mem", [&]
		{
			g.attribute("address", "0xfd690000");
			g.attribute("size",    "0x1000");
		});
		g.node("io_mem", [&]
		{
			g.attribute("address", "0xfd6a0000");
			g.attribute("size",    "0x1000");
		});
		g.node("io_mem", [&]
		{
			g.attribute("address", "0xfd6d0000");
			g.attribute("size",    "0x1000");
		});
		g.node("io_mem", [&]
		{
			g.attribute("address", "0xfd6e0000");
			g.attribute("size",    "0x1000");
		});
	});

	/*
	 * Starlabs StarLite Touchscreen IRQ
	 */
	g.node("device", [&]
	{
		g.attribute("name", "STARLITE_TOUCH");
		g.attribute("type", "acpi");
		g.node("irq", [&]
		{
			g.attribute("number", 104U);
			g.attribute("mode", "level");
			g.attribute("polarity", "low");
		});
	});

	/*
	 * Intel Meteorlake PCH Pinctrl/GPIO
	 */
	g.node("device", [&]
	{
		g.attribute("name", "INTC1083");
		g.attribute("type", "acpi");
		g.node("irq", [&]
		{
			g.attribute("number", 14U);
			g.attribute("mode", "level");
			g.attribute("polarity", "low");
		});
		g.node("io_mem", [&]
		{
			g.attribute("address", "0xe0d50000");
			g.attribute("size",    "0x1000");
		});
		g.node("io_mem", [&]
		{
			g.attribute("address", "0xe0d40000");
			g.attribute("size",    "0x1000");
		});
		g.node("io_mem", [&]
		{
			g.attribute("address", "0xe0d30000");
			g.attribute("size",    "0x1000");
		});
		g.node("io_mem", [&]
		{
			g.attribute("address", "0xe0d20000");
			g.attribute("size",    "0x1000");
		});
		g.node("io_mem", [&]
		{
			g.attribute("address", "0xe0d10000");
			g.attribute("size",    "0x1000");
		});
	});
}


Main::Intel_opregion Main::parse_intel_opregion(Pci::Config const &device)
{
	struct Opregion : Mmio<0x3c6>
	{
		struct Minor : Register<0x16, 8> { };
		struct Major : Register<0x17, 8> { };
		struct MBox  : Register<0x58, 32> {
			struct Asle : Bitfield<2, 1> { };
		};
		struct Asle_ardy : Register<0x300, 32> { };
		struct Asle_rvda : Register<0x3ba, 64> { };
		struct Asle_rvds : Register<0x3c2, 32> { };

		Opregion(Byte_range_ptr const &range) : Mmio(range) { }
	};

	addr_t const phys_asls = device.read<Mmio<0x100>::Register<0xfc, 32>>(); /* ASLS */
	if (!phys_asls)
		return { };

	size_t asls_size = 2 * 4096 /* OPREGION_SIZE */;

	try {
		Attached_io_mem_dataspace map_asls(env, phys_asls, asls_size);

		if (!map_asls.cap().valid())
			return { };

		Opregion opregion({map_asls.local_addr<char>(), asls_size});

		auto const rvda = opregion.read<Opregion::Asle_rvda>();
		auto const rvds = opregion.read<Opregion::Asle_rvds>();

		if (opregion.read<Opregion::MBox::Asle>() &&
		    opregion.read<Opregion::Major>() >= 2 && rvda && rvds) {

			/* 2.0 rvda is physical, 2.1+ rvda is relative offset */
			if (opregion.read<Opregion::Major>() > 2 ||
			    opregion.read<Opregion::Minor>() >= 1) {

				if (rvda > asls_size)
					asls_size += size_t(rvda - asls_size);
				asls_size += opregion.read<Opregion::Asle_rvds>();
			} else {
				warning("rvda/rvds unsupported case");
			}
		}

		return { phys_asls, asls_size };
	} catch (Attached_dataspace::Invalid_dataspace()) {
	} catch (Attached_dataspace::Region_conflict()) {
	} catch (Opregion::Range_violation) { }

	return { };
}


void Main::parse_pci_config_spaces(Node const &node, Generator &g)
{
	unsigned host_bridge_num = 0;

	node.for_each_sub_node("bdf", [&] (Node const &node)
	{
		addr_t const start = node.attribute_value("start",  0UL);
		addr_t const base  = node.attribute_value("base",   0UL);
		size_t const count = node.attribute_value("count",  0UL);

		bus_t const bus_off  = (bus_t) (start / FUNCTION_PER_BUS_MAX);
		bus_t const last_bus = (bus_t)
			(max(1UL, (count / FUNCTION_PER_BUS_MAX)) - 1);

		if (host_bridge_num++) {
			error("We do not support multiple host bridges by now!");
			return;
		}

		new (heap) Bridge(bridge_registry, { bus_off, 0, 0 },
		                  bus_off, last_bus);

		bus_t bus = 0;
		bus_t max_subordinate_bus = bus;
		do {
			enum { BUS_SIZE = DEVICES_PER_BUS_MAX * FUNCTION_PER_DEVICE_MAX
			                  * FUNCTION_CONFIG_SPACE_SIZE };
			addr_t offset = base + bus * BUS_SIZE;
			pci_config_ds.construct(env, offset, BUS_SIZE);
			bus_t const subordinate_bus =
				parse_pci_bus((bus_t)bus + bus_off,
				              {pci_config_ds->local_addr<char>(), BUS_SIZE},
				              offset, g);

			max_subordinate_bus = max(max_subordinate_bus, subordinate_bus);
		} while (bus++ < max_subordinate_bus);

		pci_config_ds.destruct();
	});
}


Main::Main(Env &env) : env(env)
{
	platform_info.node().with_optional_sub_node("kernel", [&] (Node const &node)
	{
		apic_capable = node.attribute_value("acpi", false);
		msi_capable  = node.attribute_value("msi",  false);
	});

	Attached_rom_dataspace sys_rom(env, "system");
	sys_rom.update();

	/*
	 * Wait until the system ROM is available
	 */
	if (!sys_rom.valid()) {
		struct Io_dummy { void fn() {}; } io_dummy;
		Io_signal_handler<Io_dummy> handler(env.ep(), io_dummy, &Io_dummy::fn);
		sys_rom.sigh(handler);
		while (!sys_rom.valid()) {
			env.ep().wait_and_dispatch_one_io_signal();
			sys_rom.update();
		}
	}

	Node const &node = sys_rom.node();

	if (apic_capable) {

		irq_override_list.update_from_node(node,

			/* create */
			[&] (Node const &node) -> Irq_override &
			{
				return *new (heap)
					Irq_override(node.attribute_value<uint8_t>("irq",   0xff),
					             node.attribute_value<uint8_t>("gsi",   0xff),
					             node.attribute_value<uint8_t>("flags", 0));
			},

			/* destroy */
			[&] (Irq_override &irq_override) { destroy(heap, &irq_override); },

			/* update */
			[&] (Irq_override &, Node const &) { }
		);

		irq_routing_list.update_from_node(node,

			/* create */
			[&] (Node const &node) -> Irq_routing &
			{
				rid_t bridge_bdf = node.attribute_value<rid_t>("bridge_bdf", 0xff);
				return *new (heap)
					Irq_routing(Bdf::bdf(bridge_bdf),
					            node.attribute_value<uint8_t>("device",     0xff),
					            node.attribute_value<uint8_t>("device_pin", 0xff),
					            node.attribute_value<uint8_t>("gsi",        0xff));
			},

			/* destroy */
			[&] (Irq_routing &irq_routing) { destroy(heap, &irq_routing); },

			/* update */
			[&] (Irq_routing &, Node const &) { }
		);
	}

	reserved_memory_list.update_from_node(node,

		/* create */
		[&] (Node const &node) -> Rmrr &
		{
			bus_t  bus = 0;
			dev_t  dev = 0;
			func_t fn  = 0;
			addr_t start = node.attribute_value("start", 0UL);
			addr_t end   = node.attribute_value("end", 0UL);

			node.with_optional_sub_node("scope", [&] (Node const &node) {
				bus = node.attribute_value<uint8_t>("bus_start", 0U);
				node.with_optional_sub_node("path", [&] (Node const &node) {
					dev = node.attribute_value<uint8_t>("dev", 0);
					fn  = node.attribute_value<uint8_t>("func", 0);
				});
			});

			return *new (heap) Rmrr({bus, dev, fn}, start, (end-start+1));
		},

		/* destroy */
		[&] (Rmrr &rmrr) { destroy(heap, &rmrr); },

		/* update */
		[&] (Rmrr &, Node const &) { }
	);

	ioapic_list.update_from_node(node,

		/* create */
		[&] (Node const &node) -> Ioapic &
		{
			uint8_t  id   = node.attribute_value("id", (uint8_t)0U);
			addr_t   addr = node.attribute_value("addr", 0U);
			uint32_t base = node.attribute_value("base_irq", 0U);

			return *new (heap) Ioapic(id, addr, base);
		},

		/* destroy */
		[&] (Ioapic &ioapic) { destroy(heap, &ioapic); },

		/* update */
		[&] (Ioapic &, Node const &) { }
	);

	unsigned nbr { 0 };
	drhd_list.update_from_node(node,

		/* create */
		[&] (Node const &node) -> Drhd &
		{
			addr_t   addr  = node.attribute_value("phys", 0UL);
			size_t   size  = node.attribute_value("size", 0UL);
			unsigned seg   = node.attribute_value("segment", 0U);
			unsigned flags = node.attribute_value("flags", 0U);

			Drhd * drhd;
			if (flags & 0x1)
				drhd = new (heap) Drhd(addr, size, seg,
				                       Drhd::Scope::INCLUDE_PCI_ALL, nbr++);
			else
				drhd = new (heap) Drhd(addr, size, seg,
				                       Drhd::Scope::EXPLICIT, nbr++);

			/* parse device scopes which define the explicitly assigned devices */
			bus_t   bus  = 0;
			dev_t   dev  = 0;
			func_t  fn   = 0;
			uint8_t type = 0;
			uint8_t id   = 0;

			node.for_each_sub_node("scope", [&] (Node const &node) {
				bus  = node.attribute_value<uint8_t>("bus_start", 0U);
				type = node.attribute_value<uint8_t>("type", 0U);
				id   = node.attribute_value<uint8_t>("id", 0U);
				node.with_optional_sub_node("path", [&] (Node const &node) {
					dev = node.attribute_value<uint8_t>("dev", 0);
					fn  = node.attribute_value<uint8_t>("func", 0);
				});

				new (heap) Drhd::Device(drhd->devices, {bus, dev, fn}, type, id);
			});

			return *drhd;
		},

		/* destroy */
		[&] (Drhd &drhd)
		{
			drhd.devices.for_each([&] (Drhd::Device &device) {
				destroy(heap, &device); });
			destroy(heap, &drhd);
		},

		/* update */
		[&] (Drhd &, Node const &) { }

	);

	pci_reporter.generate([&] (Generator &g)
	{
		parse_pci_config_spaces(node, g);

		/*
		 * Generate the ACPI device info after the PCI config space parsing,
		 * because additional information is used from there, like Intel's
		 * opregion information used by ACPICA
		 */
		parse_acpi_device_info(node, g);
	});
}


void Component::construct(Genode::Env &env) { static Main main(env); }
