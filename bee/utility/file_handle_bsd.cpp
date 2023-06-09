#include <bee/nonstd/format.h>
#include <bee/utility/file_handle.h>
#include <sys/file.h>
#include <unistd.h>

namespace bee {
    file_handle file_handle::lock(const fs::path& filename) noexcept {
        int fd = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            return {};
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) == -1) {
            ::close(fd);
            return {};
        }
        return { fd };
    }
}
