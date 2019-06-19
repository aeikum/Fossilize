/* Copyright (c) 2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <signal.h>
#include "fossilize_external_replayer.hpp"

static bool write_all(HANDLE file, const char *str)
{
	size_t len = strlen(str);
	while (len)
	{
		DWORD wrote;
		if (!WriteFile(file, str, len, &wrote, nullptr))
			return false;
		if (!FlushFileBuffers(file))
			return false;
		if (wrote <= 0)
			return false;

		str += wrote;
		len -= wrote;
	}

	return true;
}

namespace Global
{
static unordered_set<Hash> faulty_spirv_modules;
static unsigned active_processes;
static ThreadedReplayer::Options base_replayer_options;
static vector<const char *> databases;
static VulkanDevice::Options device_options;
static bool quiet_slave;
static SharedControlBlock *control_block;
static const char *shm_name;
static const char *shm_mutex_name;
static HANDLE shared_mutex;
static HANDLE job_handle;
}

struct ProcessProgress
{
	unsigned start_graphics_index = 0u;
	unsigned start_compute_index = 0u;
	unsigned end_graphics_index = ~0u;
	unsigned end_compute_index = ~0u;
	HANDLE process = nullptr;
	HANDLE crash_file_handle = INVALID_HANDLE_VALUE;
	HANDLE timer_handle = nullptr;
	HANDLE pipe_event = nullptr;

	OVERLAPPED overlapped_pipe = {};
	char async_pipe_buffer[1024];

	int compute_progress = -1;
	int graphics_progress = -1;

	bool process_once();
	bool process_shutdown();
	bool start_child_process();
	void parse(const char *cmd);
	bool kick_overlapped_io();

	uint32_t index = 0;
};

bool ProcessProgress::kick_overlapped_io()
{
	memset(&overlapped_pipe, 0, sizeof(overlapped_pipe));
	overlapped_pipe.hEvent = pipe_event;

	// The PIPE_TYPE_MESSSAGE mode makes sure that we read messages one at a time and not random binary data,
	// so it's safe to do a large read here.
	if (!ReadFile(crash_file_handle, async_pipe_buffer, sizeof(async_pipe_buffer), nullptr, &overlapped_pipe) &&
	    GetLastError() != ERROR_IO_PENDING)
	{
		return false;
	}

	return true;
}

void ProcessProgress::parse(const char *cmd)
{
	if (strncmp(cmd, "CRASH", 5) == 0)
	{
		// We crashed ... Set up a timeout in case the process hangs while trying to recover.
		if (timer_handle)
		{
			CloseHandle(timer_handle);
			timer_handle = nullptr;
		}

		timer_handle = CreateWaitableTimer(nullptr, TRUE, nullptr);
		if (timer_handle)
		{
			LARGE_INTEGER due_time;
			due_time.QuadPart = -10000000ll;
			if (!SetWaitableTimer(timer_handle, &due_time, 0, nullptr, nullptr, 0))
				LOGE("Failed to set waitable timer.\n");
		}
		else
			LOGE("Failed to create waitable timer.\n");
	}
	else if (strncmp(cmd, "GRAPHICS", 8) == 0)
		graphics_progress = int(strtol(cmd + 8, nullptr, 0));
	else if (strncmp(cmd, "COMPUTE", 7) == 0)
		compute_progress = int(strtol(cmd + 7, nullptr, 0));
	else if (strncmp(cmd, "MODULE", 6) == 0)
	{
		auto hash = strtoull(cmd + 6, nullptr, 16);
		Global::faulty_spirv_modules.insert(hash);

		if (Global::control_block)
		{
			Global::control_block->banned_modules.fetch_add(1, std::memory_order_relaxed);
			char buffer[ControlBlockMessageSize] = {};
			strcpy(buffer, cmd);

			if (WaitForSingleObject(Global::shared_mutex, INFINITE) == WAIT_OBJECT_0)
			{
				shared_control_block_write(Global::control_block, buffer, sizeof(buffer));
				ReleaseMutex(Global::shared_mutex);
			}
		}
	}
	else
		LOGE("Got unexpected message from child: %s\n", cmd);
}

bool ProcessProgress::process_once()
{
	if (crash_file_handle == INVALID_HANDLE_VALUE)
		return false;

	DWORD did_read = 0;

	// The event should be reset when calling kick_overlapped_io, but we might fail before that.
	if (!ResetEvent(pipe_event))
	{
		LOGE("Failed to reset event.\n");
		return false;
	}

	if (!GetOverlappedResult(crash_file_handle, &overlapped_pipe, &did_read, TRUE))
		return false;

	if (did_read < sizeof(async_pipe_buffer))
	{
		async_pipe_buffer[did_read] = '\0';
		parse(async_pipe_buffer);
		//LOGE("Parsed: %s", async_pipe_buffer);
		if (!kick_overlapped_io())
		{
			LOGE("Failed to kick overlapped IO.\n");
			return false;
		}
		return true;
	}
	else
		return false;
}

bool ProcessProgress::process_shutdown()
{
	// Flush out all messages we got.
	while (process_once());
	if (crash_file_handle != INVALID_HANDLE_VALUE)
		CloseHandle(crash_file_handle);
	crash_file_handle = INVALID_HANDLE_VALUE;

	// Close some handles.
	if (timer_handle)
	{
		CloseHandle(timer_handle);
		timer_handle = nullptr;
	}

	if (pipe_event)
	{
		CloseHandle(pipe_event);
		pipe_event = nullptr;
	}

	// Reap child process.
	DWORD code = 0;
	if (process)
	{
		if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0)
			return false;
		if (!GetExitCodeProcess(process, &code))
			LOGE("Failed to get exit code of process.\n");
		CloseHandle(process);
		process = nullptr;
		Global::active_processes--;
	}

	// If application exited in normal manner, we are done.
	if (code == 0)
		return false;

	// We might have crashed, but we never saw any progress marker.
	// We do not know what to do from here, so we just terminate.
	if (graphics_progress < 0 || compute_progress < 0)
	{
		LOGE("Child process terminated before we could receive progress. Cannot continue.\n");
		if (Global::control_block)
			Global::control_block->dirty_process_deaths.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	if (Global::control_block)
		Global::control_block->clean_process_deaths.fetch_add(1, std::memory_order_relaxed);

	start_graphics_index = uint32_t(graphics_progress);
	start_compute_index = uint32_t(compute_progress);
	if (start_graphics_index >= end_graphics_index && start_compute_index >= end_compute_index)
	{
		LOGE("Process index %u crashed, but there is nothing more to replay.\n", index);
		return false;
	}
	else
	{
		LOGE("Process index %u crashed, but will retry.\n", index);
		LOGE("  New graphics range (%u, %u)\n", start_graphics_index, end_graphics_index);
		LOGE("  New compute range (%u, %u)\n", start_compute_index, end_compute_index);
		return true;
	}
}

static void send_faulty_modules_and_close(HANDLE file)
{
	for (auto &m : Global::faulty_spirv_modules)
	{
		char buffer[18];
		sprintf(buffer, "%llx\n", static_cast<unsigned long long>(m));
		write_all(file, buffer);
	}

	CloseHandle(file);
}

static bool CreateCustomPipe(HANDLE *read_pipe, HANDLE *write_pipe, LPSECURITY_ATTRIBUTES attrs, bool overlapped_read)
{
	// This is a very unfortunate detail of this implementation.
	// Windows does not support using WaitFor*Object on Pipes, so CreatePipe() is out the window.
	// We have to use overlapped I/O to make this work in practice.
	// When we have overlapped I/O on pipes, we can use CreateEvent to get WaitFor*() working.
	// We also really want to use PIPE_TYPE_MESSAGE here.
	// This is so that we can safely read one message at a time with ReadFile rather than rely on fgets to delimit each message for us.
	static unsigned pipe_serial;
	char pipe_name_buffer[MAX_PATH];
	sprintf(pipe_name_buffer, "\\\\.\\Pipe\\Fossilize.%08lx.%08x", GetCurrentProcessId(), pipe_serial++);
	*read_pipe = CreateNamedPipeA(pipe_name_buffer, PIPE_ACCESS_INBOUND | (overlapped_read ? FILE_FLAG_OVERLAPPED : 0),
	                              PIPE_TYPE_MESSAGE | PIPE_WAIT | PIPE_READMODE_MESSAGE, 1, 4096, 4096, 10000, attrs);

	if (*read_pipe == INVALID_HANDLE_VALUE)
		return false;

	*write_pipe = CreateFileA(pipe_name_buffer, GENERIC_WRITE, 0, attrs, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (*write_pipe == INVALID_HANDLE_VALUE)
	{
		CloseHandle(*read_pipe);
		return false;
	}

	return true;
}

bool ProcessProgress::start_child_process()
{
	graphics_progress = -1;
	compute_progress = -1;

	if (start_graphics_index >= end_graphics_index && start_compute_index >= end_compute_index)
	{
		// Nothing to do.
		return true;
	}

	// We cannot use fork() on Windows, so we need to create a new process which references ourselves.

	char filename[MAX_PATH];
	if (FAILED(GetModuleFileNameA(nullptr, filename, sizeof(filename))))
		return false;

	std::string cmdline;
	cmdline += "\"";
	cmdline += filename;
	cmdline += "\"";

	for (auto &path : Global::databases)
	{
		cmdline += " \"";
		cmdline += path;
		cmdline += "\"";
	}

	cmdline += " --slave-process";
	cmdline += " --num-threads 1";
	cmdline += " --graphics-pipeline-range ";
	cmdline += to_string(start_graphics_index);
	cmdline += " ";
	cmdline += to_string(end_graphics_index);
	cmdline += " --compute-pipeline-range ";
	cmdline += to_string(start_compute_index);
	cmdline += " ";
	cmdline += to_string(end_compute_index);

	if (Global::shm_name)
	{
		cmdline += " --shm-name ";
		cmdline += Global::shm_name;
	}

	if (Global::shm_mutex_name)
	{
		cmdline += " --shm-mutex-name ";
		cmdline += Global::shm_mutex_name;
	}

	if (Global::base_replayer_options.pipeline_cache)
		cmdline += " --pipeline-cache";
	if (Global::base_replayer_options.spirv_validate)
		cmdline += " --spirv-val";

	if (!Global::base_replayer_options.on_disk_pipeline_cache_path.empty())
	{
		cmdline += " --on-disk-pipeline-cache ";
		cmdline += "\"";
		cmdline += Global::base_replayer_options.on_disk_pipeline_cache_path;
		if (index != 0)
		{
			cmdline += ".";
			cmdline += std::to_string(index);
		}
		cmdline += "\"";
		// TODO: Merge the on-disk pipeline caches, but it's probably not that important.
		// We're supposed to populate the driver caches here first and foremost.
	}

	// Create custom named pipes which can be inherited by our child processes.
	SECURITY_ATTRIBUTES attrs = {};
	attrs.bInheritHandle = TRUE;
	attrs.nLength = sizeof(SECURITY_ATTRIBUTES);
	attrs.lpSecurityDescriptor = nullptr;

	HANDLE slave_stdout_read = INVALID_HANDLE_VALUE;
	HANDLE slave_stdout_write = INVALID_HANDLE_VALUE;
	HANDLE master_stdout_read = INVALID_HANDLE_VALUE;
	HANDLE master_stdout_write = INVALID_HANDLE_VALUE;

	if (!CreateCustomPipe(&slave_stdout_read, &master_stdout_write, &attrs, false))
	{
		LOGE("Failed to create pipe.\n");
		return false;
	}

	if (!CreateCustomPipe(&master_stdout_read, &slave_stdout_write, &attrs, true))
	{
		LOGE("Failed to create pipe.\n");
		return false;
	}

	// These files should only live in the master process.
	if (!SetHandleInformation(master_stdout_read, HANDLE_FLAG_INHERIT, 0))
	{
		LOGE("Failed to set handle information.\n");
		return false;
	}

	if (!SetHandleInformation(master_stdout_write, HANDLE_FLAG_INHERIT, 0))
	{
		LOGE("Failed to set handle information.\n");
		return false;
	}

	STARTUPINFO si = {};
	si.cb = sizeof(STARTUPINFO);
	si.hStdOutput = slave_stdout_write;
	si.hStdInput = slave_stdout_read;
	si.dwFlags |= STARTF_USESTDHANDLES;
	PROCESS_INFORMATION pi = {};
	HANDLE nul = INVALID_HANDLE_VALUE;

	if (Global::quiet_slave)
	{
		nul = CreateFileA("NUL", GENERIC_WRITE, 0, &attrs, OPEN_EXISTING, 0, nullptr);
		if (nul == INVALID_HANDLE_VALUE)
		{
			LOGE("Failed to open NUL file for writing.\n");
			return false;
		}

		si.hStdError = nul;
	}
	else
	{
		if (!SetHandleInformation(GetStdHandle(STD_ERROR_HANDLE), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
		{
			LOGE("Failed to enable inheritance for stderror handle.\n");
			return false;
		}
		si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	}

	// For whatever reason, this string must be mutable. Dupe it.
	char *duped_string = _strdup(cmdline.c_str());
	if (!CreateProcessA(nullptr, duped_string, nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
	{
		LOGE("Failed to create child process.\n");
		free(duped_string);
		return false;
	}
	free(duped_string);

	if (Global::job_handle && !AssignProcessToJobObject(Global::job_handle, pi.hProcess))
	{
		LOGE("Failed to assign process to job handle.\n");
		// This isn't really fatal, just continue on.
	}

	// Now we can resume the main thread, after we've added the process to our job object.
	ResumeThread(pi.hThread);

	// Close file handles which are used by the child process only.
	CloseHandle(slave_stdout_read);
	CloseHandle(slave_stdout_write);
	if (nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);

	// Don't need the thread handle.
	CloseHandle(pi.hThread);
	process = pi.hProcess;

	// Send over which SPIR-V modules should be ignored.
	send_faulty_modules_and_close(master_stdout_write);

	crash_file_handle = master_stdout_read;
	Global::active_processes++;

	pipe_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!pipe_event)
	{
		LOGE("Failed to create event.\n");
		return false;
	}

	// Kick an asynchronous read from the pipe here.
	// The pipe_event will signal once something happens.
	if (!kick_overlapped_io())
	{
		LOGE("Failed to start overlapped I/O.\n");
		return false;
	}

	return true;
}

static void log_and_die()
{
	DWORD dw = GetLastError();
	char *lpMsgBuf = nullptr;
	FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr,
		dw,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		reinterpret_cast<LPSTR>(&lpMsgBuf), 0, nullptr);

	LOGE("Error: %s\n", lpMsgBuf);
	ExitProcess(1);
}

static bool open_shm(const char *shm_path, const char *shm_mutex_path)
{
	HANDLE mapping = OpenFileMapping(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, shm_path);
	if (!mapping)
	{
		LOGE("Failed to open file mapping in replayer.\n");
		return false;
	}

	void *mapped = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);

	if (!mapped)
	{
		LOGE("Failed to map view of file in replayer.\n");
		return false;
	}

	const auto is_pot = [](size_t size) { return (size & (size - 1)) == 0; };
	// Detect some obvious shenanigans.
	Global::control_block = static_cast<SharedControlBlock *>(mapped);
	if (Global::control_block->version_cookie != ControlBlockMagic ||
	    Global::control_block->ring_buffer_offset < sizeof(SharedControlBlock) ||
	    Global::control_block->ring_buffer_size == 0 ||
	    !is_pot(Global::control_block->ring_buffer_size))
	{
		LOGE("Control block is corrupt.\n");
		UnmapViewOfFile(mapped);
		CloseHandle(mapping);
		Global::control_block = nullptr;
	}

	Global::shared_mutex = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, shm_mutex_path);
	if (!Global::shared_mutex)
		return false;

	return true;
}

static int run_master_process(const VulkanDevice::Options &opts,
                              const ThreadedReplayer::Options &replayer_opts,
                              const vector<const char *> &databases,
                              bool quiet_slave, const char *shm_name, const char *shm_mutex_name)
{
	Global::quiet_slave = quiet_slave;
	Global::device_options = opts;
	Global::base_replayer_options = replayer_opts;
	Global::databases = databases;
	unsigned processes = replayer_opts.num_threads;
	Global::base_replayer_options.num_threads = 1;
	Global::shm_name = shm_name;
	Global::shm_mutex_name = shm_mutex_name;

	Global::job_handle = CreateJobObjectA(nullptr, nullptr);
	if (!Global::job_handle)
	{
		LOGE("Failed to create job handle.\n");
		// Not fatal, we just won't bother with this.
	}

	if (Global::job_handle)
	{
		// Kill all child processes if the parent dies.
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
		jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!SetInformationJobObject(Global::job_handle, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
		{
			LOGE("Failed to set information for job object.\n");
			// Again, not fatal.
		}
	}

	if (shm_name && shm_mutex_name && !open_shm(shm_name, shm_mutex_name))
	{
		LOGE("Failed to map external memory resources.\n");
		return EXIT_FAILURE;
	}

	size_t num_graphics_pipelines;
	size_t num_compute_pipelines;
	{
		auto db = create_database(databases);
		if (!db->prepare())
		{
			for (auto &path : databases)
				LOGE("Failed to parse database %s.\n", path);
			return EXIT_FAILURE;
		}

		if (!db->get_hash_list_for_resource_tag(RESOURCE_GRAPHICS_PIPELINE, &num_graphics_pipelines, nullptr))
		{
			for (auto &path : databases)
				LOGE("Failed to parse database %s.\n", path);
			return EXIT_FAILURE;
		}

		if (!db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE, &num_compute_pipelines, nullptr))
		{
			for (auto &path : databases)
				LOGE("Failed to parse database %s.\n", path);
			return EXIT_FAILURE;
		}
	}

	if (Global::control_block)
		Global::control_block->progress_started.store(1, std::memory_order_release);

	Global::active_processes = 0;
	vector<ProcessProgress> child_processes(processes);
	vector<HANDLE> wait_handles;

	// CreateProcess for our children.
	for (unsigned i = 0; i < processes; i++)
	{
		auto &progress = child_processes[i];
		progress.start_graphics_index = (i * unsigned(num_graphics_pipelines)) / processes;
		progress.end_graphics_index = ((i + 1) * unsigned(num_graphics_pipelines)) / processes;
		progress.start_compute_index = (i * unsigned(num_compute_pipelines)) / processes;
		progress.end_compute_index = ((i + 1) * unsigned(num_compute_pipelines)) / processes;
		progress.index = i;
		if (!progress.start_child_process())
		{
			LOGE("Failed to start child process.\n");
			return EXIT_FAILURE;
		}
	}

	wait_handles.reserve(3 * processes);

	while (Global::active_processes != 0)
	{
		wait_handles.clear();

		// Per process, three events can trigger:
		// - Process HANDLE is signalled, the child process completed.
		// - pipe_event is signalled, an asynchronous read operation on the pipe completed.
		// - Timer event is signalled. This marks a timeout on the process which is trying to crash gracefully.
		for (auto &process : child_processes)
		{
			// Prioritize that any file I/O events are signalled before process end.
			// WaitForMultipleObjects must return the first object which is signalled.
			if (process.pipe_event)
				wait_handles.push_back(process.pipe_event);
			if (process.process)
				wait_handles.push_back(process.process);
			if (process.timer_handle)
				wait_handles.push_back(process.timer_handle);
		}

		// Basically like poll(), except we had to a lot of work to get here ...
		DWORD ret = WaitForMultipleObjects(wait_handles.size(), wait_handles.data(), FALSE, INFINITE);
		if (ret == WAIT_FAILED)
		{
			LOGE("WaitForMultipleObjects failed.\n");
			log_and_die();
			return EXIT_FAILURE;
		}
		else if (ret == WAIT_TIMEOUT)
			continue;
		else if (ret >= WAIT_ABANDONED_0)
			continue;
		else if (ret >= WAIT_OBJECT_0 && ret < WAIT_OBJECT_0 + wait_handles.size())
		{
			// Handle the event accordingly.
			HANDLE handle = wait_handles[ret - WAIT_OBJECT_0];
			auto process_itr = find_if(begin(child_processes), end(child_processes), [&](const ProcessProgress &prog) {
				return prog.process == handle;
			});

			auto event_itr = find_if(begin(child_processes), end(child_processes), [&](const ProcessProgress &prog) {
				return prog.pipe_event == handle;
			});

			auto timer_itr = find_if(begin(child_processes), end(child_processes), [&](const ProcessProgress &prog) {
				return prog.timer_handle == handle;
			});

			if (process_itr != end(child_processes))
			{
				// The process finished.
				if (process_itr->process_shutdown() && !process_itr->start_child_process())
				{
					LOGE("Failed to start child process.\n");
					return EXIT_FAILURE;
				}
			}
			else if (event_itr != end(child_processes))
			{
				// Read a command, and queue up a new async read.
				event_itr->process_once();
			}
			else if (timer_itr != end(child_processes))
			{
				LOGE("Terminating process due to timeout ...\n");
				if (!TerminateProcess(timer_itr->process, 3))
				{
					LOGE("Failed to terminate child process.\n");
					return EXIT_FAILURE;
				}

				// Process should terminate immediately, so clean up here.
				if (timer_itr->process_shutdown() && !timer_itr->start_child_process())
				{
					LOGE("Failed to start child process.\n");
					return EXIT_FAILURE;
				}
			}
		}
	}

	if (Global::job_handle)
		CloseHandle(Global::job_handle);

	if (Global::control_block)
		Global::control_block->progress_complete.store(1, std::memory_order_release);

	return EXIT_SUCCESS;
}

static ThreadedReplayer *global_replayer = nullptr;
static HANDLE crash_handle;

static LONG WINAPI crash_handler(_EXCEPTION_POINTERS *)
{
	// stderr is reserved for generic logging.
	// stdout/stdin is for IPC with master process.

	if (!write_all(crash_handle, "CRASH\n"))
		ExitProcess(2);

	// This might hang indefinitely if we are exceptionally unlucky,
	// the parent will have a timeout after receiving the crash message.
	// If that fails, it can TerminateProcess us.
	// We want to make sure any database writing threads in the driver gets a chance to complete its work
	// before we die.

	if (global_replayer)
	{
		char buffer[32];

		// Report to parent process which VkShaderModule's might have contributed to our untimely death.
		// This allows a new process to ignore these modules.
		for (unsigned i = 0; i < global_replayer->num_failed_module_hashes; i++)
		{
			sprintf(buffer, "MODULE %llx\n",
					static_cast<unsigned long long>(global_replayer->failed_module_hashes[i]));
			if (!write_all(crash_handle, buffer))
				ExitProcess(2);
		}

		// Report where we stopped, so we can continue.
		sprintf(buffer, "GRAPHICS %u\n", global_replayer->get_per_thread_data().current_graphics_index);
		if (!write_all(crash_handle, buffer))
			ExitProcess(2);

		sprintf(buffer, "COMPUTE %u\n", global_replayer->get_per_thread_data().current_compute_index);
		if (!write_all(crash_handle, buffer))
			ExitProcess(2);

		global_replayer->emergency_teardown();
	}

	// Clean exit instead of reporting the segfault.
	// Use exit code 2 to mark a segfaulted child.
	ExitProcess(2);
	return EXCEPTION_EXECUTE_HANDLER;
}

static void abort_handler(int)
{
	crash_handler(nullptr);
}

static int run_slave_process(const VulkanDevice::Options &opts,
                             const ThreadedReplayer::Options &replayer_opts,
                             const vector<const char *> &databases, const char *shm_name, const char *shm_mutex_name)
{
	if (shm_name && shm_mutex_name && !open_shm(shm_name, shm_mutex_name))
	{
		LOGE("Failed to map external memory resources.\n");
		return EXIT_FAILURE;
	}

	auto tmp_opts = replayer_opts;
	tmp_opts.control_block = Global::control_block;
	ThreadedReplayer replayer(opts, tmp_opts);
	replayer.robustness = true;

	// In slave mode, we can receive a list of shader module hashes we should ignore.
	// This is to avoid trying to replay the same faulty shader modules again and again.
	char ignored_shader_module_hash[16 + 2];
	while (fgets(ignored_shader_module_hash, sizeof(ignored_shader_module_hash), stdin))
	{
		errno = 0;
		auto hash = strtoull(ignored_shader_module_hash, nullptr, 16);
		if (hash == 0)
			break;
		if (errno == 0)
		{
			//LOGE("Ignoring module %llx\n", hash);
			replayer.mask_shader_module(Hash(hash));
		}
	}

	// Make sure that the driver or some other agent cannot write to stdout and confuse the master process.
	if (!DuplicateHandle(
		GetCurrentProcess(),
		GetStdHandle(STD_OUTPUT_HANDLE),
		GetCurrentProcess(),
		&crash_handle,
		DUPLICATE_SAME_ACCESS,
		FALSE, DUPLICATE_CLOSE_SOURCE))
	{
		LOGE("Failed to duplicate stdout handle.\n");
		log_and_die();
	}

	// Setup a last resort SEH handler. This overrides any global messagebox saying "application crashed",
	// which is what we want.
	SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
	SetUnhandledExceptionFilter(crash_handler);
	// This is needed to catch abort() on Windows.
	// It cannot really catch abort() from debug programs, since the abort() handler in the CRT creates its own message box.
	// Release code however, is caught just fine.
	signal(SIGABRT, abort_handler);

	global_replayer = &replayer;
	int code = run_normal_process(replayer, databases);
	global_replayer = nullptr;

	// Do not try to catch errors in teardown. Crashes here should never happen, and if they do,
	// it's very sketchy to attempt to catch them, since the crash handler will likely try to refer to
	// data which does not exist anymore.
	signal(SIGABRT, SIG_DFL);
	SetErrorMode(0);
	SetUnhandledExceptionFilter(nullptr);

#if 0
	if (Global::control_block)
	{
		if (WaitForSingleObject(Global::shared_mutex, INFINITE) == WAIT_OBJECT_0)
		{
			char msg[ControlBlockMessageSize] = {};
			sprintf(msg, "SLAVE_FINISHED\n");
			shared_control_block_write(Global::control_block, msg, sizeof(msg));
			ReleaseMutex(Global::shared_mutex);
		}
	}
#endif

	return code;
}
