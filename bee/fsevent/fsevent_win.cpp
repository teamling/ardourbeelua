#include <bee/fsevent/fsevent_win.h>
#include <bee/utility/unicode.h>
#include <bee/utility/format.h>
#include <bee/exception/windows_exception.h>
#include <array>
#include <assert.h>

namespace bee::win::fsevent {
	class task : public OVERLAPPED {
		static const size_t kBufSize = 16 * 1024;
	public:
		task(watch* watch, taskid id);
		~task();

		bool   open(const std::wstring& path);
		bool   start();
		void   cancel();
		taskid getid();
		void   push_notify(tasktype type, std::wstring&& message);
		void   remove();
		void   changes_cb(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered);

	private:
		watch*                    m_watch;
		taskid                        m_id;
		fs::path                      m_path;
		HANDLE                        m_directory;
		std::array<uint8_t, kBufSize> m_buffer;
		std::array<uint8_t, kBufSize> m_bakbuffer;
	};

	static void __stdcall filewatch_apc_cb(ULONG_PTR arg) {
		watch* self = (watch*)arg;
		self->apc_cb();
	}

	static void __stdcall fwtask_changes_cb(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
		task* self = (task*)lpOverlapped->hEvent;
		self->changes_cb(dwErrorCode, dwNumberOfBytesTransfered);
	}

	task::task(watch* watch, taskid id)
		: m_watch(watch)
		, m_id(id)
		, m_path()
		, m_directory(INVALID_HANDLE_VALUE)
	{
		memset(this, 0, sizeof(OVERLAPPED));
		hEvent = this;
	}

	task::~task() {
		assert(m_directory == INVALID_HANDLE_VALUE);
	}

	bool task::open(const std::wstring& path) {
		if (m_directory != INVALID_HANDLE_VALUE) {
			return true;
		}
		std::error_code ec;
		m_path = fs::absolute(path, ec);
		if (ec) {
			push_notify(tasktype::Error, bee::format(L"`std::filesystem::absolute` failed: %s", ec.message()));
			return false;
		}
		DWORD sharemode = FILE_SHARE_READ | FILE_SHARE_WRITE;
		m_directory = ::CreateFileW(m_path.c_str(),
			FILE_LIST_DIRECTORY,
			sharemode,
			NULL,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
			NULL);
		if (m_directory == INVALID_HANDLE_VALUE) {
			push_notify(tasktype::Error, bee::format(L"`CreateFileW` failed: %s", error_message()));
			return false;
		}
		return true;
	}

	void task::cancel() {
		if (m_directory != INVALID_HANDLE_VALUE) {
			::CancelIo(m_directory);
			::CloseHandle(m_directory);
			m_directory = INVALID_HANDLE_VALUE;
		}
	}

	taskid task::task::getid() {
		return m_id;
	}

	void task::remove() {
		if (m_watch) {
			m_watch->removetask(this);
		}
	}

	bool task::start() {
		assert(m_directory != INVALID_HANDLE_VALUE);
		if (!::ReadDirectoryChangesW(
			m_directory,
			&m_buffer[0],
			static_cast<DWORD>(m_buffer.size()),
			TRUE,
			FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
			FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_LAST_ACCESS | FILE_NOTIFY_CHANGE_CREATION,
			NULL,
			this,
			&fwtask_changes_cb))
		{
			push_notify(tasktype::Error, bee::format(L"`ReadDirectoryChangesW` failed: %s", error_message()));
			return false;
		}
		return true;
	}

	void task::changes_cb(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered) {
		if (dwErrorCode == ERROR_OPERATION_ABORTED) {
			remove();
			return;
		}
		if (!dwNumberOfBytesTransfered) {
			return;
		}
		assert(dwNumberOfBytesTransfered >= offsetof(FILE_NOTIFY_INFORMATION, FileName) + sizeof(WCHAR));
		assert(dwNumberOfBytesTransfered <= m_bakbuffer.size());
		memcpy(&m_bakbuffer[0], &m_buffer[0], dwNumberOfBytesTransfered);
		start();

		uint8_t* data = m_bakbuffer.data();
		for (;;) {
			FILE_NOTIFY_INFORMATION& fni = (FILE_NOTIFY_INFORMATION&)*data;
			std::wstring path(fni.FileName, fni.FileNameLength / sizeof(wchar_t));
			switch (fni.Action) {
			case FILE_ACTION_ADDED:
				push_notify(tasktype::Create, (m_path / path).wstring());
				break;
			case FILE_ACTION_REMOVED:
				push_notify(tasktype::Delete, (m_path / path).wstring());
				break;
			case FILE_ACTION_MODIFIED:
				push_notify(tasktype::Modify, (m_path / path).wstring());
				break;
			case FILE_ACTION_RENAMED_OLD_NAME:
			case FILE_ACTION_RENAMED_NEW_NAME:
				push_notify(tasktype::Rename, (m_path / path).wstring());
				break;
			default:
				assert(false);
				break;
			}
			if (!fni.NextEntryOffset) {
				break;
			}
			data += fni.NextEntryOffset;
		}
	}

	void task::push_notify(tasktype type, std::wstring&& message) {
		m_watch->m_notify.push({
			type,
			std::forward<std::wstring>(message),
		});
	}

	watch::watch()
		: m_thread()
		, m_tasks()
		, m_notify()
		, m_terminate(false)
		, m_gentask(kInvalidTaskId)
	{ }

	watch::~watch() {
		stop();
		assert(m_tasks.empty());
	}

	void watch::removetask(task* t) {
		if (t) {
			auto it = m_tasks.find(t->getid());
			if (it != m_tasks.end()) {
				m_tasks.erase(it);
			}
		}
	}

	bool watch::thread_signal() {
		return !!::QueueUserAPC(filewatch_apc_cb, m_thread->native_handle(), (ULONG_PTR)this);
	}

	bool watch::thread_init() {
		if (m_thread) {
			return true;
		}
		m_thread.reset(new std::thread(std::bind(&watch::thread_cb, this)));
		return true;
	}

	void watch::thread_cb() {
		while (!m_terminate || !m_tasks.empty()) {
			::SleepEx(INFINITE, true);
		}
	}

	void watch::stop() {
		if (!m_thread) {
			return;
		}
		if (!m_thread->joinable()) {
			m_thread.reset();
			return;
		}
		m_apc_queue.push({
			apc_arg::type::Terminate
		});
		if (!thread_signal()) {
			m_thread->detach();
			m_thread.reset();
			return;
		}
		m_thread->join();
		m_thread.reset();
	}

	taskid watch::add(const std::wstring& path) {
		if (!thread_init()) {
			return kInvalidTaskId;
		}
		taskid id = ++m_gentask;
		m_apc_queue.push({
			apc_arg::type::Add,
			id,
			path
		});
		thread_signal();
		return id;
	}

	bool watch::remove(taskid id) {
		if (!m_thread) {
			return false;
		}
		m_apc_queue.push({
			apc_arg::type::Remove,
			id
		});
		thread_signal();
		return true;
	}

	void watch::apc_cb() {
		apc_arg arg;
		while (m_apc_queue.pop(arg)) {
			switch (arg.m_type) {
			case apc_arg::type::Add:
				apc_add(arg.m_id, arg.m_path);
				break;
			case apc_arg::type::Remove:
				apc_remove(arg.m_id);
				break;
			case apc_arg::type::Terminate:
				apc_terminate();
				return;
			}
		}
	}

	void watch::apc_add(taskid id, const std::wstring& path) {
		auto t = std::make_shared<task>(this, id);
		if (!t->open(path)) {
			return;
		}
		m_tasks.insert(std::make_pair(id, t));
		t->start();
	}

	void watch::apc_remove(taskid id) {
		auto it = m_tasks.find(id);
		if (it != m_tasks.end()) {
			it->second->cancel();
		}
	}

	void watch::apc_terminate() {
		if (m_tasks.empty()) {
			return;
		}
		std::vector<std::shared_ptr<task>> tmp;
		for (auto& it : m_tasks) {
			tmp.push_back(it.second);
		}
		for (auto& it : tmp) {
			it->cancel();
		}
		m_terminate = true;
	}

	bool watch::select(notify& n) {
		return m_notify.pop(n);
	}
}