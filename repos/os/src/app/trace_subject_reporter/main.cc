/*
 * \brief  Report information about present trace subjects
 * \author Norman Feske
 * \date   2015-06-15
 */

/*
 * Copyright (C) 2015-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <base/attached_rom_dataspace.h>
#include <trace_session/connection.h>
#include <timer_session/connection.h>
#include <base/component.h>
#include <base/attached_rom_dataspace.h>
#include <base/heap.h>
#include <os/reporter.h>
#include <util/retry.h>
#include <base/log.h>
#include <util/reconstructible.h>


struct Trace_subject_registry
{
	private:

		struct Entry : Genode::List<Entry>::Element
		{
			Genode::Trace::Subject_id const id;

			Genode::Trace::Subject_info info { };

			/**
			 * Execution time during the last period
			 */
			unsigned long long recent_execution_time = 0;

			Entry(Genode::Trace::Subject_id id) : id(id) { }

			void update(Genode::Trace::Subject_info const &new_info)
			{
				unsigned long long const last_execution_time = info.execution_time().thread_context;
				info = new_info;
				recent_execution_time = info.execution_time().thread_context - last_execution_time;
			}
		};

		Genode::List<Entry> _entries { };

		Entry *_lookup(Genode::Trace::Subject_id const id)
		{
			for (Entry *e = _entries.first(); e; e = e->next())
				if (e->id == id)
					return e;

			return nullptr;
		}

		static constexpr Genode::Trace::Num_subjects MAX_SUBJECTS { 512 };

		Genode::Trace::Subject_id _subjects[MAX_SUBJECTS.value];

		void _sort_by_recent_execution_time()
		{
			Genode::List<Entry> sorted;

			while (_entries.first()) {

				/* find entry with lowest recent execution time */
				Entry *lowest = _entries.first();
				for (Entry *e = _entries.first(); e; e = e->next()) {
					if (e->recent_execution_time < lowest->recent_execution_time)
						lowest = e;
				}

				_entries.remove(lowest);
				sorted.insert(lowest);
			}

			_entries = sorted;
		}

	public:

		Genode::Trace::Connection::For_each_subject_info_result
		update(Genode::Trace::Connection &trace, Genode::Allocator &alloc)
		{
			return trace.for_each_subject_info([&] (Genode::Trace::Subject_id id,
			                                 Genode::Trace::Subject_info const &info) {

				Entry *e = _lookup(id);
				if (!e) {
					e = new (alloc) Entry(id);
					_entries.insert(e);
				}

				e->update(info);

				/* purge dead threads */
				if (e->info.state() == Genode::Trace::Subject_info::DEAD) {
					trace.free(e->id);
					_entries.remove(e);
					Genode::destroy(alloc, e);
				}
			});

			_sort_by_recent_execution_time();
		}

		void report(Genode::Generator &g,
		            bool report_affinity, bool report_activity)
		{
			for (Entry const *e = _entries.first(); e; e = e->next()) {
				g.node("subject", [&] () {
					g.attribute("label", e->info.session_label().string());
					g.attribute("thread", e->info.thread_name().string());
					g.attribute("id", e->id.id);

					using Subject_info = Genode::Trace::Subject_info;
					Subject_info::State const state = e->info.state();
					g.attribute("state", Subject_info::state_name(state));

					if (report_activity)
						g.node("activity", [&] () {
							g.attribute("total", e->info.execution_time().thread_context);
							g.attribute("recent", e->recent_execution_time);
						});

					if (report_affinity)
						g.node("affinity", [&] () {
							g.attribute("xpos", e->info.affinity().xpos());
							g.attribute("ypos", e->info.affinity().ypos());
						});
				});
			}
		}
};


namespace App {

	struct Main;
	using namespace Genode;
}


struct App::Main
{
	Env &_env;

	/*
	 * THE ARGUMENT BUFFER IS A CEILING ON HOW MANY SUBJECTS CAN BE REPORTED,
	 * and 32 KiB is about 131 of them -- Carrier Detect Labs, 2026-11-09.
	 *
	 * core clamps its reply to what fits (base/src/core/
	 * trace_session_component.cc:56-62), so on a machine with more threads
	 * than that the report is SILENTLY TRUNCATED. It cost a measurement: a
	 * capture of a Hercules partition reported 131 subjects against a highest
	 * subject id of 187, and the threads it dropped included the emulated
	 * processor -- the one the measurement was about. 57 of 90 seconds of one
	 * core's time was attributed to nothing, which was only noticed because
	 * the consumer summed each core's subjects and found they did not add up
	 * to the span.
	 *
	 * Both sizes are configurable now, with the old values as defaults.
	 */
	static size_t _default_ram()        { return 128*1024; }
	static size_t _default_arg_buffer() { return  32*1024; }

	size_t _ram        = _default_ram();
	size_t _arg_buffer = _default_arg_buffer();

	Constructible<Trace::Connection> _trace { };

	Expanding_reporter _reporter { _env, "trace_subjects", "trace_subjects",
	                               Expanding_reporter::Initial_buffer_size { 64*1024 } };

	static uint64_t _default_period_ms() { return 5000; }

	uint64_t _period_ms = _default_period_ms();

	bool _report_affinity = false;
	bool _report_activity = false;

	Attached_rom_dataspace _config { _env, "config" };

	bool _config_report_attribute_enabled(char const *attr) const
	{
		return _config.node().with_sub_node("report",
			[&] (Node const &node) { return node.attribute_value(attr, false); },
			[]                     { return false; });
	}

	Timer::Connection _timer { _env };

	Heap _heap { _env.ram(), _env.rm() };

	Trace_subject_registry _trace_subject_registry { };

	void _handle_config();

	Signal_handler<Main> _config_handler = {
		_env.ep(), *this, &Main::_handle_config};

	void _handle_period();

	Signal_handler<Main> _periodic_handler = {
		_env.ep(), *this, &Main::_handle_period};

	Main(Env &env) : _env(env)
	{
		_config.sigh(_config_handler);
		_handle_config();

		_timer.sigh(_periodic_handler);
	}
};


void App::Main::_handle_config()
{
	_config.update();

	_period_ms = _config.node().attribute_value("period_ms", _default_period_ms());

	_report_affinity = _config_report_attribute_enabled("affinity");
	_report_activity = _config_report_attribute_enabled("activity");

	/*
	 * THE TRACE SESSION IS OPENED ONCE AND NOT REOPENED ON A CONFIG CHANGE.
	 * Its buffer sizes are read at the first configuration because a new
	 * session would renumber every subject and discard every `total`
	 * accumulated so far, which would silently reset a measurement in
	 * progress. A change to either size after start-up is therefore reported
	 * and ignored rather than applied.
	 */
	size_t const ram = _config.node().attribute_value("ram", _default_ram());
	size_t const arg = _config.node().attribute_value("arg_buffer",
	                                                  _default_arg_buffer());
	if (!_trace.constructed()) {
		_ram = ram; _arg_buffer = arg;
		_trace.construct(_env, _ram, _arg_buffer);
		log("trace session: ram ", _ram/1024, "K, argument buffer ",
		    _arg_buffer/1024, "K, which holds about ",
		    _arg_buffer / (sizeof(Trace::Subject_info) +
		                   sizeof(Trace::Subject_id)), " subject(s)");
	} else if (ram != _ram || arg != _arg_buffer) {
		warning("ram/arg_buffer changed after start-up and are IGNORED: "
		        "reopening the trace session would renumber every subject "
		        "and discard every accumulated total");
	}

	_timer.trigger_periodic(1000*_period_ms);
}


void App::Main::_handle_period()
{
	/* update subject information */
	Trace::Connection::For_each_subject_info_result const info =
		_trace_subject_registry.update(*_trace, _heap);

	/*
	 * AND SAY SO WHEN THE LIST DID NOT FIT. core returns the number it could
	 * put in the buffer together with the number the buffer holds, and this
	 * component used to discard both -- so a truncated report was
	 * indistinguishable from a complete one. `count == limit` means the reply
	 * filled the buffer exactly, which is the only evidence available that
	 * there may be more.
	 */
	if (info.count && info.count >= info.limit)
		warning("trace subject list filled the argument buffer: ",
		        info.count, " of at most ", info.limit, " subject(s) -- the "
		        "report is probably TRUNCATED. Raise arg_buffer.");

	/* generate report */
	_reporter.generate([&] (Generator &g) {
		/*
		 * The two numbers go in the DOCUMENT and not only in the log,
		 * because a consumer that reads the report over a wire has no way to
		 * see a warning, and a truncated document that cannot say so is the
		 * failure this change exists to remove.
		 */
		g.attribute("subjects", info.count);
		g.attribute("capacity", info.limit);
		_trace_subject_registry.report(g, _report_affinity, _report_activity);
	});
}


void Component::construct(Genode::Env &env) { static App::Main main(env); }

