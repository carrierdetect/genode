/*
 * \brief  Monitored execution in main context
 * \author Christian Helmuth
 * \author Christian Prochaska
 * \date   2020-01-09
 */

/*
 * Copyright (C) 2020 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _LIBC__INTERNAL__MONITOR_H_
#define _LIBC__INTERNAL__MONITOR_H_

/* Genode includes */
#include <base/registry.h>
#include <trace/timestamp.h>
#include <util/reconstructible.h>

/* libc-internal includes */
#include <internal/types.h>

namespace Libc { class Blockade; };


/*
 * THE MONITOR QUEUE, INSTRUMENTED.
 *
 * Why this exists. A libc component whose entrypoint also serves a VFS plugin
 * that talks to a peer on a busy CPU can see a monitor round trip cost
 * microseconds or milliseconds from the same thread, microseconds apart: two
 * consecutive zero-timeout polls in one thread of an emulated communications
 * controller measured 2,918 us and 19.4 us under such a load. Everything
 * outside this file had been ruled out by then -- the calling thread, the
 * phase of the caller's loop, and the timeout of the poll that preceded the
 * call -- which leaves the state of THIS queue when the entrypoint finished
 * the previous job. That is not observable from outside, so it is counted
 * here.
 *
 * What is counted, per completed synchronous job, is how many examination
 * PASSES elapsed between the job being enqueued and the entrypoint executing
 * it, and how many other jobs the entrypoint got through in that pass before
 * reaching it. A job that is executed by the pass its own
 * 'trigger_monitor_examination' caused waited zero passes; one that waited
 * three sat through three batches of other components' work. Binned by passes
 * waited, with the elapsed time and the queue position accumulated per bin,
 * that distinguishes "the round trip is expensive" from "the round trip was
 * behind other work", which no measurement in the caller can do.
 *
 * DECLARED HERE, DEFINED IN kernel.cc. This header is included by nine
 * translation units; file-scope state in it would be nine separate copies,
 * each counting a fraction of the traffic and none of them the symbol the
 * reader resolves.
 *
 * WRITERS. Everything except 'monitor_passes' is written only by the
 * entrypoint, in 'execute_monitors'. 'monitor_passes' is read without a lock
 * by each enqueueing thread, and the reader exported from kernel.cc resets the
 * bins from whichever thread asks. Both races are benign for an instrument and
 * neither can corrupt a pointer; a torn read costs one sample.
 *
 * COST. Two 'Trace::timestamp()' reads per job. That primitive serialises with
 * 'cpuid' and costs about 163 cycles on the hardware this was written for --
 * four times a bare 'rdtscp' -- which at the few thousand jobs a second a busy
 * component runs is under a millisecond of CPU per second of wall clock. The
 * framework's own abstraction is used rather than a hand-rolled 'rdtscp'
 * because this file is built for every architecture and that one is not.
 */
namespace Libc {

	enum { MONITOR_BINS = 6 };   /* passes waited: 0, 1, 2, 3, 4-7, 8+ */

	extern unsigned long long monitor_passes;      /* examination passes */
	extern unsigned long long monitor_examined;    /* jobs looked at, all passes */
	extern unsigned long long monitor_completed;   /* jobs completed */
	extern unsigned long long monitor_examined_max;/* largest single pass */
	extern unsigned long long monitor_bin_count[MONITOR_BINS];
	extern unsigned long long monitor_bin_tsc  [MONITOR_BINS];
	extern unsigned long long monitor_bin_max  [MONITOR_BINS];
	extern unsigned long long monitor_bin_ahead[MONITOR_BINS];

	inline unsigned monitor_bin(unsigned long long waited)
	{
		if (waited < 4) return (unsigned)waited;
		return (waited < 8) ? 4 : 5;
	}
}


class Libc::Blockade
{
	protected:

		bool _woken_up { false };
		bool _expired  { false };

	public:

		bool woken_up() const { return _woken_up; }
		bool expired()  const { return _expired; }

		virtual void block()  = 0;
		virtual void wakeup() = 0;
};


namespace Libc { class Monitor; };


class Libc::Monitor : Interface
{
	public:

		enum class Result { COMPLETE, TIMEOUT };

		struct Job;
		struct Pool;

		enum class Function_result { COMPLETE, INCOMPLETE };

		struct Function : Interface { virtual Function_result execute() = 0; };

	protected:

		virtual Result _monitor(Function &, uint64_t) = 0;
		virtual void _trigger_monitor_examination() = 0;

	public:

		/**
		 * Block until monitored execution completed or timeout expires
		 *
		 * Returns true if execution completed, false on timeout.
		 */
		template <typename FN>
		Result monitor(FN const &fn, uint64_t timeout_ms = 0)
		{
			struct _Function : Function
			{
				FN const &fn;
				Function_result execute() override { return fn(); }
				_Function(FN const &fn) : fn(fn) { }
			} function { fn };

			return _monitor(function, timeout_ms);
		}

		/**
		 * Monitor asynchronous job execution
		 *
		 * Returns immediately after the job is registered for execution.
		 */
		virtual void monitor_async(Job &job) = 0;

		/**
		 * Trigger examination of monitored functions
		 */
		void trigger_monitor_examination() { _trigger_monitor_examination(); }
};


class Libc::Monitor::Job
{
	private:

		friend class Pool;

		Monitor::Function &_fn;
		Blockade          &_blockade;

		/*
		 * The enqueue stamp for the queue instrument above. ~0 means "never
		 * stamped", which is how an ASYNCHRONOUS job is left: it is registered
		 * and executed whenever its I/O says so, and the passes between those
		 * two moments are not a queueing delay anyone is waiting on.
		 */
		unsigned long long _enq_pass { ~0ULL };
		unsigned long long _enq_tsc  { 0 };

		Constructible<Registry<Job>::Element> _async_element;

		void _register_async(Registry<Job> &registry)
		{
			_async_element.construct(registry, *this);
		}

	public:

		Job(Monitor::Function &fn, Blockade &blockade)
		: _fn(fn), _blockade(blockade) { }

		virtual ~Job()
		{
			if (_async_element.constructed())
				_async_element.destruct();
		}

		bool execute() { return _fn.execute() == Function_result::COMPLETE; }

		bool completed() const { return _blockade.woken_up(); }
		bool expired()   const { return _blockade.expired(); }

		void wait_for_completion() { _blockade.block(); }
		void complete()            { _blockade.wakeup(); }
};


struct Libc::Monitor::Pool
{
	private:

		Monitor       &_monitor;
		Registry<Job>  _jobs;

	public:

		Pool(Monitor &monitor) : _monitor(monitor) { }

		/**
		 * Monitor synchronous job execution
		 *
		 * The function is called by the monitor-user context and returns after
		 * job completion.
		 */
		void monitor(Job &job)
		{
			/*
			 * Stamped BEFORE the element is constructed, because constructing
			 * it is what makes the job reachable by the entrypoint -- stamping
			 * afterwards would race with a pass that has already executed it.
			 */
			job._enq_pass = monitor_passes;
			job._enq_tsc  = Genode::Trace::timestamp();

			Registry<Job>::Element element { _jobs, job };

			_monitor.trigger_monitor_examination();

			job.wait_for_completion();
		}

		/**
		 * Monitor asynchronous job execution
		 *
		 * The function is called by the monitor-user context and returns after
		 * job is registered for execution. Jobs are removed from the pool on
		 * destruction.
		 */
		void monitor_async(Job &job)
		{
			job._register_async(_jobs);
		}

		enum class State { JOBS_PENDING, ALL_COMPLETE };

		/* called by the monitor context itself */
		State execute_monitors()
		{
			State result = State::ALL_COMPLETE;

			unsigned long long const pass = ++monitor_passes;
			unsigned long long       examined = 0;

			_jobs.for_each([&] (Job &job) {

				if (!job.completed() && !job.expired()) {

					unsigned long long const ahead = examined++;

					bool const completed = job.execute();

					if (completed) {

						/*
						 * ACCOUNT BEFORE 'complete()', NOT AFTER. 'complete()'
						 * wakes the caller, and for a synchronous job the Job
						 * object is that caller's stack local -- reading it
						 * afterwards is a use-after-free that would usually
						 * get away with it.
						 */
						if (job._enq_pass != ~0ULL) {

							/*
							 * A job enqueued by another thread DURING this
							 * pass can carry this pass's own number, so the
							 * subtraction is clamped rather than allowed to
							 * wrap into the last bin.
							 */
							unsigned long long const waited =
								(pass > job._enq_pass) ? pass - 1 - job._enq_pass : 0;

							unsigned long long const dt =
								Genode::Trace::timestamp() - job._enq_tsc;

							unsigned const b = monitor_bin(waited);

							monitor_bin_count[b]++;
							monitor_bin_tsc  [b] += dt;
							monitor_bin_ahead[b] += ahead;
							if (dt > monitor_bin_max[b])
								monitor_bin_max[b] = dt;
						}

						monitor_completed++;

						job.complete();
					}

					if (!completed)
						result = State::JOBS_PENDING;
				}
			});

			monitor_examined += examined;
			if (examined > monitor_examined_max)
				monitor_examined_max = examined;

			return result;
		}
};

#endif /* _LIBC__INTERNAL__MONITOR_H_ */
