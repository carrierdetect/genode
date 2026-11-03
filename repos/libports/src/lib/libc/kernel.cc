/*
 * \brief  Libc kernel for main and pthreads user contexts
 * \author Christian Helmuth
 * \author Emery Hemingway
 * \author Norman Feske
 * \date   2016-01-22
 */

/*
 * Copyright (C) 2016-2024 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* libc-internal includes */
#include <internal/kernel.h>
#include <internal/file_operations.h>

Libc::Kernel * Libc::Kernel::_kernel_ptr;


extern char **environ;

char const *libc_resolv_path; /* expected by res_init.c */

/**
 * Blockade for main context
 */

inline void Libc::Main_blockade::block()
{
	Check check { _woken_up };

	do {
		_timeout_ms = Kernel::kernel().suspend(check, _timeout_ms);
		_expired    = _timeout_valid && !_timeout_ms;
	} while (!woken_up() && !expired());
}

inline void Libc::Main_blockade::wakeup()
{
	_woken_up = true;
	Kernel::kernel().resume_main();
}


void Libc::Kernel::reset_malloc_heap()
{
	_malloc_ram.construct(_heap, _env.ram());

	_cloned_heap_ranges.for_each([&] (Registered<Cloned_malloc_heap_range> &r) {
		destroy(_heap, &r); });

	Heap &raw_malloc_heap = *_malloc_heap;
	construct_at<Heap>(&raw_malloc_heap, *_malloc_ram, _env.rm());

	reinit_malloc(raw_malloc_heap);
}


void Libc::Kernel::_init_file_descriptors()
{
	using Path_element_token = Genode::Token<Vfs::Scanner_policy_path_element>;

	/* guard used to print an offending libc config when leaving the scope */
	struct Diag_guard
	{
		Kernel &kernel;
		bool show = false;

		Diag_guard(Kernel &kernel) : kernel(kernel) { }

		~Diag_guard() { if (show) log(kernel._config_rom.node()); }

	} diag_guard { *this };

	auto resolve_symlinks = [&] (Absolute_path next_iteration_working_path,
	                             Absolute_path &resolved_path) -> Symlink_resolve_result
	{
		char path_element[PATH_MAX];
		char symlink_target[PATH_MAX];

		Absolute_path current_iteration_working_path;

		enum { FOLLOW_LIMIT = 10 };
		int follow_count = 0;
		bool symlink_resolved_in_this_iteration;
		do {
			if (follow_count++ == FOLLOW_LIMIT) {
				errno = ELOOP;
				return Symlink_resolve_error();
			}

			current_iteration_working_path = next_iteration_working_path;

			next_iteration_working_path.import("");
			symlink_resolved_in_this_iteration = false;

			Path_element_token t(current_iteration_working_path.base());

			while (t) {
				if (t.type() != Path_element_token::IDENT) {
						t = t.next();
						continue;
				}

				t.string(path_element, sizeof(path_element));

				try {
					next_iteration_working_path.append_element(path_element);
				} catch (Path_base::Path_too_long) {
					errno = ENAMETOOLONG;
					return Symlink_resolve_error();
				}

				/*
				 * If a symlink has been resolved in this iteration, the remaining
				 * path elements get added and a new iteration starts.
				 */
				if (!symlink_resolved_in_this_iteration) {
					struct stat stat_buf;
					int res = _vfs.stat_from_kernel(next_iteration_working_path.base(), &stat_buf);
					if (res == -1) {
						return Symlink_resolve_error();
					}
					if (S_ISLNK(stat_buf.st_mode)) {
						res = readlink(next_iteration_working_path.base(),
						               symlink_target, sizeof(symlink_target) - 1);
						if (res < 1)
							return Symlink_resolve_error();

						/* zero terminate target */
						symlink_target[res] = 0;

						if (symlink_target[0] == '/')
							/* absolute target */
							next_iteration_working_path.import(symlink_target, _cwd.base());
						else {
							/* relative target */
							next_iteration_working_path.strip_last_element();
							try {
								next_iteration_working_path.append_element(symlink_target);
							} catch (Path_base::Path_too_long) {
								errno = ENAMETOOLONG;
								return Symlink_resolve_error();
							}
						}
						symlink_resolved_in_this_iteration = true;
					}
				}

				t = t.next();
			}

		} while (symlink_resolved_in_this_iteration);

		resolved_path = next_iteration_working_path;
		resolved_path.remove_trailing('/');

		return Ok();
	};

	using Path = String<Vfs::MAX_PATH_LEN>;

	struct Absolute_path_resolve_error { };
	using Absolute_path_resolve_result = Attempt<Ok, Absolute_path_resolve_error>;

	auto resolve_absolute_path = [&] (Path const &path, Absolute_path &abs_path) -> Absolute_path_resolve_result
	{
		Absolute_path abs_dir(path.string(), _cwd.base());   abs_dir.strip_last_element();
		Absolute_path dir_entry(path.string(), _cwd.base()); dir_entry.keep_only_last_element();

		try {
			if (resolve_symlinks(abs_dir, abs_path).failed())
				return Absolute_path_resolve_error();
			abs_path.append_element(dir_entry.string());
			return Ok();
		} catch (Path_base::Path_too_long) { return Absolute_path_resolve_error(); }
	};

	auto init_fd = [&] (Node const &node, char const *attr,
	                    int libc_fd, unsigned flags)
	{
		if (!node.has_attribute(attr))
			return;

		Path const attr_value { node.attribute_value(attr, Path()) };

		Absolute_path path { };

		if (resolve_absolute_path(attr_value, path).failed()) {
			warning("failed to resolve path for ", path);
			diag_guard.show = true;
			return;
		}

		struct stat out_stat { };
		if (_vfs.stat_from_kernel(path.string(), &out_stat) != 0) {
			warning("failed to call 'stat' on ", path);
			diag_guard.show = true;
			return;
		}

		File_descriptor *fd =
			_vfs.open_from_kernel(path.string(), flags, libc_fd);

		if (!fd)
			return;

		if (fd->libc_fd != libc_fd) {
			error("could not allocate fd ",libc_fd," for ",path,", "
			      "got fd ",fd->libc_fd);
			_vfs.close_from_kernel(fd);
			diag_guard.show = true;
			return;
		}

		fd->cloexec = node.attribute_value("cloexec", false);

		/*
		 * We need to manually register the path. Normally this is done
		 * by '_open'. But we call the local 'open' function directly
		 * because we want to explicitly specify the libc fd ID.
		 */
		if (fd->fd_path)
			warning("may leak former FD path memory");

		{
			char *dst = (char *)_heap.alloc(path.max_len());
			copy_cstring(dst, path.string(), path.max_len());
			fd->fd_path = dst;
		}

		::off_t const seek = node.attribute_value("seek", 0ULL);
		if (seek)
			_vfs.lseek_from_kernel(fd, seek);
	};

	if (_vfs.root_dir_has_dirents()) {

		_config_rom.node().with_optional_sub_node("libc", [&] (Node const &node) {

			using Path = String<Vfs::MAX_PATH_LEN>;

			if (node.has_attribute("cwd"))
				_cwd.import(node.attribute_value("cwd", Path()).string(), _cwd.base());

			init_fd(node, "stdin",  0, O_RDONLY);
			init_fd(node, "stdout", 1, O_WRONLY);
			init_fd(node, "stderr", 2, O_RDWR);

			node.for_each_sub_node("fd", [&] (Node const &fd) {

				unsigned const id = fd.attribute_value("id", 0U);

				bool const rd = fd.attribute_value("readable",  false);
				bool const wr = fd.attribute_value("writeable", false);

				unsigned const flags = rd ? (wr ? O_RDWR : O_RDONLY)
				                          : (wr ? O_WRONLY : 0);

				if (!fd.has_attribute("path")) {
					warning("unknown path for file descriptor ", id);
					diag_guard.show = true;
				}

				init_fd(fd, "path", id, flags);
			});
		});

		/* prevent use of IDs of stdin, stdout, and stderr for other files */
		for (unsigned fd = 0; fd <= 2; fd++)
			_fd_alloc.preserve(fd);
	}

	/**
	 * Call 'fn' with root directory and path to ioctl pseudo file as arguments
	 *
	 * If no matching ioctl pseudo file exists, 'fn' is not called.
	 */
	auto with_ioctl_path = [&] (File_descriptor const *fd, char const *file, auto fn)
	{
		if (!fd || !fd->fd_path)
			return;

		Absolute_path const ioctl_dir = Vfs_plugin::ioctl_dir(*fd);
		Absolute_path path = ioctl_dir;
		path.append_element(file);

		_vfs.with_root_dir([&] (Directory &root_dir) {
			if (root_dir.file_exists(path.string()))
				fn(root_dir, path.string()); });
	};

	/*
	 * Watch stdout's 'info' pseudo file to detect terminal-resize events
	 */
	File_descriptor const * const stdout_fd =
		_fd_alloc.find_by_libc_fd(STDOUT_FILENO);

	with_ioctl_path(stdout_fd, "info", [&] (Directory &root_dir, char const *path) {
		_terminal_resize_handler.construct(root_dir, path, *this,
		                                   &Kernel::_handle_terminal_resize); });

	/*
	 * Watch stdin's 'interrupts' pseudo file to detect control-c events
	 */
	File_descriptor const * const stdin_fd =
		_fd_alloc.find_by_libc_fd(STDIN_FILENO);

	with_ioctl_path(stdin_fd, "interrupts", [&] (Directory &root_dir, char const *path) {
		_user_interrupt_handler.construct(root_dir, path,
		                                  *this, &Kernel::_handle_user_interrupt); });
}


void Libc::Kernel::_handle_terminal_resize()
{
	_signal.charge(SIGWINCH);
	_resume_main();
}


void Libc::Kernel::_handle_user_interrupt()
{
	_signal.charge(SIGINT);
	_resume_main();
}


void Libc::Kernel::_clone_state_from_parent()
{
	using Range = Region_map::Range;

	auto range_attr = [&] (Node const &node)
	{
		return Range {
			.start     = node.attribute_value("at",   0UL),
			.num_bytes = node.attribute_value("size", 0UL)
		};
	};

	/*
	 * Allocate local memory for the backing store of the application heap,
	 * mirrored from the parent.
	 *
	 * This step must precede the creation of the 'Clone_connection' because
	 * the shared-memory buffer of the clone session may otherwise potentially
	 * interfere with such a heap region.
	 */
	_with_libc_config([&] (Node const &libc) {
		libc.for_each_sub_node("heap", [&] (Node const &node) {
			Range const range = range_attr(node);
			new (_heap)
				Registered<Cloned_malloc_heap_range>(_cloned_heap_ranges,
				                                     _env.ram(), _env.rm(),
				                                     range); });
	});

	_clone_connection.construct(_env);

	/* fetch heap content */
	_cloned_heap_ranges.for_each([&] (Cloned_malloc_heap_range &heap_range) {
		heap_range.import_content(*_clone_connection); });

	/* value of global environ pointer (the env vars are already on the heap) */
	_clone_connection->memory_content(&environ, sizeof(environ));

	/* fetch user contex of the parent's application */
	_clone_connection->memory_content(&_user_context, sizeof(_user_context));
	_clone_connection->memory_content(&_main_monitor_job, sizeof(_main_monitor_job));
	_valid_user_context = true;

	auto copy_from_parent = [&] (Range range)
	{
		_clone_connection->memory_content((void *)range.start, range.num_bytes);
	};

	_with_libc_config([&] (Node const &libc) {
		libc.for_each_sub_node([&] (Node const &node) {

			/* clone application stack */
			if (node.type() == "stack")
				copy_from_parent(range_attr(node));

			/* clone RW segment of a shared library or the binary */
			if (node.type() == "rw") {
				using Name = String<64>;
				Name const name = node.attribute_value("name", Name());

				/*
				 * The blacklisted segments are initialized via the
				 * regular startup of the child.
				 */
				bool const blacklisted = (name == "ld.lib.so")
				                      || (name == "libc.lib.so")
				                      || (name == "libm.lib.so")
				                      || (name == "posix.lib.so")
				                      || (strcmp(name.string(), "vfs", 3) == 0);
				if (!blacklisted)
					copy_from_parent(range_attr(node));
			}
		});
	});

	/* import application-heap state from parent */
	_clone_connection->object_content(_malloc_heap);
	init_malloc_cloned(*_clone_connection);
}


void Libc::Kernel::handle_io_progress()
{
	if (_io_progressed) {
		_io_progressed = false;

		Kernel::resume_all();

		if (_execute_monitors_pending == Monitor::Pool::State::JOBS_PENDING)
			_execute_monitors_pending = _monitors.execute_monitors();
	}

	wakeup_remote_peers();
}


void Libc::execute_in_application_context(Application_code &app_code)
{
	/*
	 * The libc execution model builds on the main entrypoint, which handles
	 * all relevant signals (e.g., timing and VFS). Additional component
	 * entrypoints or pthreads should never call with_libc() but we catch this
	 * here and just execute the application code directly.
	 */
	if (!Kernel::kernel().main_context()) {
		app_code.execute();
		Kernel::kernel().wakeup_remote_peers();
		return;
	}

	static bool nested = false;

	if (nested) {

		if (Kernel::kernel().main_suspended()) {
			Kernel::kernel().nested_execution(app_code);
		} else {
			app_code.execute();
		}
		return;
	}

	nested = true;
	Kernel::kernel().run(app_code);
	nested = false;

	Kernel::kernel().wakeup_remote_peers();
}


static Libc::File_descriptor_allocator *_atexit_fd_alloc_ptr;


static void close_file_descriptors_on_exit()
{
	for (;;) {
		int const fd = _atexit_fd_alloc_ptr->any_open_fd();
		if (fd == -1)
			break;
		close(fd);
	}
}


void Libc::Kernel::reset_atexit()
{
	_atexit.reset_handlers();
	atexit(close_file_descriptors_on_exit);
}


Libc::Kernel::Kernel(Genode::Env &env, Genode::Allocator &heap)
:
	_env(env), _heap(heap)
{
	libc_resolv_path = _config.nameserver.string();

	/*
	 * The monitor-queue instrument's gate, set before anything can enqueue a
	 * job: this runs during 'Kernel' construction and the only thread in
	 * existence is this one. See internal/monitor.h.
	 */
	monitor_stats_on = _config.monitor_stats;

	init_atexit(_atexit);

	_atexit_fd_alloc_ptr = &_fd_alloc;
	atexit(close_file_descriptors_on_exit);

	init_semaphore_support(_timer_accessor);
	init_pthread_support(*this, _timer_accessor);

	_with_libc_sub_config("pthread", [&] (Node const &pthread_config) {
		init_pthread_support(env, pthread_config, _heap); });

	_env.ep().register_io_progress_handler(*this);

	if (_config.cloned) {
		_clone_state_from_parent();

	} else {
		_malloc_heap.construct(*_malloc_ram, _env.rm());
		init_malloc(*_malloc_heap);
	}

	init_fork(_env, _fd_alloc, _libc_env, _heap, *_malloc_heap, _config.pid, *this,
	          _signal, _binary_name);
	init_execve(_env, _heap, _user_stack, *this, *this, _binary_name, _fd_alloc);
	init_plugin(*this);
	init_sleep(*this);
	init_vfs_plugin(*this, _env.rm());
	init_file_operations(*this, _fd_alloc, _libc_env);
	init_pread_pwrite(_fd_alloc);
	init_time(*this, *this);
	init_alarm(_timer_accessor, _signal);
	init_poll(_signal, *this, _fd_alloc);
	init_socket_fs(*this, _fd_alloc, _config);
	init_socket_operations(_fd_alloc, _config);

	_with_libc_sub_config("passwd", [&] (Node const &passwd_config) {
		init_passwd(passwd_config); });

	_with_libc_sub_config("group", [&] (Node const &group_config) {
		init_group(group_config); });

	init_signal(_signal);
	init_kqueue(_heap, *this, _fd_alloc);
	init_random(_config);

	_init_file_descriptors();

	_kernel_ptr = this;

	/*
	 * Acknowledge the completion of 'fork' to the parent
	 *
	 * This must be done after '_init_file_descriptors' to ensure that pipe FDs
	 * of the parent are opened at the child before the parent continues.
	 * Otherwise, the parent would potentially proceed with closing the pipe
	 * FDs before the child has a chance to open them. In this situation, the
	 * pipe reference counter may reach an intermediate value of zero,
	 * triggering the destruction of the pipe.
	 */
	if (_config.cloned)
		_clone_connection.destruct();
}


/*
 * The monitor-queue instrument's state, and the one call that reads it.
 *
 * internal/monitor.h explains what is counted and why. The definitions live
 * here because that header is included by nine translation units and file-
 * scope state in it would be nine separate copies.
 */

bool               Libc::monitor_stats_on    { false };
unsigned long long Libc::monitor_passes       { 0 };
unsigned long long Libc::monitor_examined     { 0 };
unsigned long long Libc::monitor_completed    { 0 };
unsigned long long Libc::monitor_examined_max { 0 };
unsigned long long Libc::monitor_bin_count[Libc::MONITOR_BINS] { };
unsigned long long Libc::monitor_bin_tsc  [Libc::MONITOR_BINS] { };
unsigned long long Libc::monitor_bin_max  [Libc::MONITOR_BINS] { };
unsigned long long Libc::monitor_bin_ahead[Libc::MONITOR_BINS] { };


/**
 * Read and reset the monitor-queue counters
 *
 * 'bins' is the number of entries the caller has room for in each of the four
 * arrays; the return value is the number actually written, so a caller built
 * against a different bin count gets a short answer rather than a corrupted
 * stack. Each array is indexed by the number of examination passes the job
 * waited: 0, 1, 2, 3, 4-7, 8 or more.
 *
 * 'passes' is CUMULATIVE and is the one counter not reset, because it is the
 * clock each in-flight job's enqueue stamp is measured against: zeroing it
 * would make every job already queued look as though it had waited no passes
 * at all, which is precisely the bin the measurement turns on. Difference it
 * across two reads to get a window's worth. Everything else is read and
 * reset.
 *
 * One reader is the intended arrangement, as for any of this component's other
 * read-and-reset instruments; two would each see part of the traffic. The
 * reset races with the entrypoint's own increments and may lose a sample at
 * the boundary; it cannot do worse, because every counter is a plain word and
 * nothing here owns memory.
 */
extern "C" unsigned libc_monitor_stats(unsigned long long *passes,
                                       unsigned long long *examined,
                                       unsigned long long *completed,
                                       unsigned long long *examined_max,
                                       unsigned long long *bin_count,
                                       unsigned long long *bin_tsc,
                                       unsigned long long *bin_max,
                                       unsigned long long *bin_ahead,
                                       unsigned            bins)
{
	using namespace Libc;

	if (passes)       { *passes       = monitor_passes; }
	if (examined)     { *examined     = monitor_examined;     monitor_examined     = 0; }
	if (completed)    { *completed    = monitor_completed;    monitor_completed    = 0; }
	if (examined_max) { *examined_max = monitor_examined_max; monitor_examined_max = 0; }

	unsigned const n = (bins < MONITOR_BINS) ? bins : MONITOR_BINS;

	for (unsigned i = 0; i < n; i++) {
		if (bin_count) { bin_count[i] = monitor_bin_count[i]; monitor_bin_count[i] = 0; }
		if (bin_tsc)   { bin_tsc  [i] = monitor_bin_tsc  [i]; monitor_bin_tsc  [i] = 0; }
		if (bin_max)   { bin_max  [i] = monitor_bin_max  [i]; monitor_bin_max  [i] = 0; }
		if (bin_ahead) { bin_ahead[i] = monitor_bin_ahead[i]; monitor_bin_ahead[i] = 0; }
	}

	return n;
}


/**
 * Whether the per-job timing bins are being filled
 *
 * A caller needs this to tell an instrument that is OFF from one that is on
 * and saw no traffic: both return zeroed bins. The counters the flag does not
 * gate -- 'passes', 'examined', 'completed' -- are valid either way.
 */
extern "C" int libc_monitor_stats_enabled(void)
{
	return Libc::monitor_stats_on ? 1 : 0;
}
