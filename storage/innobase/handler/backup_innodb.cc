/* Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "my_global.h"
#include "sql_class.h"
#include "backup_innodb.h"
#include "log0log.h"
#include "srw_lock.h"
#include "fil0fil.h"
#include <vector>

#ifdef _WIN32
#elif defined __APPLE__
# include <sys/attr.h>
# include <sys/clonefile.h>
#else
# ifdef __linux__
#  include <sys/sendfile.h>
/* Copy a file to a stream or to a regular file. */
static inline ssize_t
send_step(int in_fd, int out_fd, size_t count, off_t *offset) noexcept
{
  return sendfile(out_fd, in_fd, offset, count);
}
# endif
# if defined __linux__ || defined __FreeBSD__
/* Copy between files in a single (type of) file system */
static inline ssize_t
copy_step(int in_fd, int out_fd, size_t count, off_t *offset) noexcept
{
  return copy_file_range(in_fd, offset, out_fd, nullptr, count, 0);
}
# endif
# ifndef __linux__
#  include <sys/mman.h>
/** Copy a file using a memory mapping.
@param in_fd   source file
@param out_fd  destination
@param count   number of bytes to copy
@return error code
@retval 0  on success
@retval 1  if a memory mapping failed */
static ssize_t mmap_copy(int in_fd, int out_fd, off_t count)
{
#if SIZEOF_SIZE_T < 8
  if (count != ssize_t(count))
    return 1;
#endif
#if 1
  return 1;
#endif
  void *p= mmap(nullptr, count, PROT_READ, MAP_SHARED, in_fd, 0);
  if (p == MAP_FAILED)
    return 1;
  ssize_t ret;
  size_t c= size_t(count);
  for (const char *b= static_cast<const char*>(p);; b+= ret)
  {
    ret= write(out_fd, b, std::min(c, size_t(INT_MAX >> 20 << 20)));
    if (ret < 0)
      break;
    c-= ret;
    if (!c)
    {
      ret= 0;
      break;
    }
    if (!ret)
    {
      ret= -1;
      break;
    }
  }
  munmap(p, count);
  return ret;
}

static ssize_t pread_write(int in_fd, int out_fd, off_t count) noexcept
{
  constexpr size_t READ_WRITE_SIZE= 65536;
  char *b= static_cast<char*>(aligned_malloc(READ_WRITE_SIZE, 4096));
  if (!b)
    return -1;
  ssize_t ret;
  for (off_t o= 0;; o+= ret)
  {
    ret= pread(in_fd, b, ssize_t(std::min(count, off_t{READ_WRITE_SIZE})), o);
    if (ret > 0)
      ret= write(out_fd, b, ret);
    if (ret < 0)
      break;
    count-= ret;
    if (!count)
    {
      ret= 0;
      break;
    }
    if (!ret)
    {
      ret= -1;
      break;
    }
  }
  aligned_free(b);
  return ret;
}
# endif

using copying_step= ssize_t(int,int,size_t,off_t*);
template<copying_step step>
static ssize_t copy(int in_fd, int out_fd, off_t c) noexcept
{
  ssize_t ret;
  for (off_t offset{0};;)
  {
    off_t count= c;
    if (count > INT_MAX >> 20 << 20)
      count = INT_MAX >> 20 << 20;
    ret= step(in_fd, out_fd, size_t(count), &offset);
    if (ret < 0)
      break;
    c-= ret;
    if (!c)
      return 0;
    if (!ret)
      return -1;
  }
  return ret;
}
#endif

namespace
{
class InnoDB_backup
{
  /** mutex protecting the queue */
  srw_mutex_impl<false> mutex;

  /** the original state of innodb_log_archive */
  bool was_archived;

  /** collection of files to be copied */
  std::vector<uint32_t> queue;

  /** target directory name or handle */
  IF_WIN(const char*,int) target;

  /** the checkpoint from which the backup starts */
  lsn_t checkpoint;
  /** end_lsn of the checkpoint at the backup start */
  lsn_t checkpoint_end_lsn;
public:
  /**
     Start of BACKUP SERVER: collect all files to be backed up
     @param thd     current session
     @param target  target directory
     @return error code
     @retval 0 on success
  */
  int init(THD *thd, IF_WIN(const char*,int) target) noexcept
  {
    mysql_mutex_lock(&LOCK_global_system_variables);
    mutex.init();
    mutex.wr_lock();
    was_archived= log_sys.archive;
    bool fail{log_sys.set_archive(true, thd)};
    mysql_mutex_unlock(&LOCK_global_system_variables);

    if (!fail)
    {
      log_sys.latch.wr_lock();
      fail= !log_sys.archive;
      checkpoint= log_sys.archived_checkpoint;
      checkpoint_end_lsn= log_sys.archived_lsn;
      log_sys.latch.wr_unlock();
    }

    if (!fail)
    {
      this->target= target;
      /* Collect all tablespaces that have been created before our
      start checkpoint. Newer tablespaces will be recovered by the
      innodb_log_archive=ON recovery.

      If a tablespace is deleted before step() is invoked, the file
      will not be copied, and a FILE_DELETE record in the log will
      ensure correct recovery.

      If a tablespace is renamed between this and end(), the recovery
      of a FILE_RENAME record will ensure the correct file name,
      no matter which name was used by step(). */
      mysql_mutex_lock(&fil_system.mutex);
      for (fil_space_t &space : fil_system.space_list)
        if (space.id < SRV_SPACE_ID_UPPER_BOUND &&
            /* FIXME: how to initialize create_lsn for old files, to
            have efficient incremental backup?
            fil_node_t::read_page0() cannot assign it from
            FIL_PAGE_LSN because that would not reflect the file
            creation but for example allocating or freeing a page.

            The easy parts of initializing space->create_lsn are
            as follows:
            (1) In log_parse_file() when processing FILE_CREATE
            (2) In deferred_spaces.create() */
            space.get_create_lsn() < checkpoint)
          queue.emplace_back(space.id);
      mysql_mutex_unlock(&fil_system.mutex);
    }
    mutex.wr_unlock();
    return fail;
  }

  /**
     Process a file that was collected at init().
     This may be invoked from multiple concurrent threads.
     @param thd   current session
     @return number of files remaining, or negative on error
     @retval 0 on completion
  */
  int step(THD *thd) noexcept
  {
    uint32_t id= FIL_NULL;
    mutex.wr_lock();
    size_t size{queue.size()};
    if (size)
    {
      size--;
      id= queue.back();
      queue.pop_back();
    }
    mutex.wr_unlock();

    if (fil_space_t *space= fil_space_t::get(id))
    {
      for (fil_node_t *node= UT_LIST_GET_FIRST(space->chain); node;
           node= UT_LIST_GET_NEXT(chain, node))
        if (int res= backup(node))
        {
          space->release();
          return res;
        }
      space->release();
    }

    size= std::min(size_t{std::numeric_limits<int>::max()}, size);

    return int(size);
  }

  /**
     Finish copying and determine the logical time of the backup snapshot.
     @param thd   current session
     @param abort whether BACKUP SERVER was aborted
     @return error code
     @retval 0 on success
  */
  int end(THD *thd, bool abort) noexcept
  {
    mutex.wr_lock();
    if (abort)
      queue.clear();
    ut_ad(queue.empty());
    mutex.wr_unlock();
    return 0;
  }

  /**
     After a successful end(), finalize the backup.
     @param thd   current session
  */
  void fini(THD *thd) noexcept
  {
    mysql_mutex_lock(&LOCK_global_system_variables);
    ut_d(mutex.wr_lock());
    ut_ad(queue.empty());
    ut_d(mutex.wr_unlock());
    mutex.destroy();
    if (!was_archived)
      log_sys.set_archive(false, thd);
    mysql_mutex_unlock(&LOCK_global_system_variables);
  }

private:
  int backup(fil_node_t *node) noexcept
  {
    /* TODO: copy the file to target safely, even when there may be
    concurrent buf_page_t::flush() to this tablespace */

#ifdef _WIN32
    std::string path{target};
    path.push_back('/');
    path.append(node->name);
    if (!CopyFileExA(node->name, path, nullptr, nullptr, false,
                     COPY_FILE_NO_BUFFERING))
    {
      /* TODO: try_mkdir */
      sql_print_error("InnoDB: BACKUP SERVER: failed to copy %s to %s,"
                      " error %iE",
                      node->name, path, GetLastError());
      return -1;
    }
#else
    bool tried_mkdir{false};
  retry:
# ifdef __APPLE__
    if (!fclonefileat(node->handle, target, node->name, 0))
      return 0;
    switch (errno) {
    case ENOENT:
      goto try_mkdir;
    case ENOTSUP:
      break;
    default:
      goto fail;
    }
# endif
    int f= openat(target, node->name, O_CREAT|O_EXCL|O_TRUNC|O_WRONLY,0666);
    if (f < 0)
    {
      if (errno == ENOENT)
      {
# ifdef __APPLE__
      try_mkdir:
# endif
        if (!tried_mkdir && node->space->id &&
            !srv_is_undo_tablespace(node->space->id))
        {
          tried_mkdir= true;
          const char *sep= strchr(node->name, '/');
          ut_ad(sep);
          sep= strchr(sep + 1, '/');
          ut_ad(sep);
          std::string dir{node->name, size_t(sep - node->name)};
          if (!mkdirat(target, dir.c_str(), 0777) || errno == EEXIST)
            goto retry;
        }
      }
    fail:
      my_error(ER_CANT_CREATE_FILE, MYF(0), node->name, errno);
      return -1;
    }
# ifdef __APPLE__
    int err=
      fcopyfile(node->handle, f, nullptr, COPYFILE_ALL | COPYFILE_CLONE);
    if (close(f) || err)
      goto fail;
    return 0;
# else
    off_t size= off_t{node->size} * node->space->physical_size();
#  if defined __linux__ || defined __FreeBSD__
    if (!copy<copy_step>(node->handle, f, size));
    else if (errno != EOPNOTSUPP || copy<send_step>(node->handle, f, size))
    {
      std::ignore= close(f);
      goto fail;
    }
#  endif
#  ifndef __linux__ // starting with Linux 2.6.33, we can rely on sendfile(2)
    ssize_t err= mmap_copy(node->handle, f, size);
    if (err == 1)
      err= pread_write(node->handle, f, size);
    if (err)
    {
      std::ignore= close(f);
      goto fail;
    }
#  endif
    if (close(f))
      goto fail;

    sql_print_information("BACKUP SERVER: copy %s", node->name);
    return 0;
# endif
#endif
  }
};

/** The backup context */
static InnoDB_backup backup;
}

int innodb_backup_start(THD *thd, IF_WIN(const char*,int) target) noexcept
{
  return backup.init(thd, target);
}

int innodb_backup_step(THD *thd) noexcept
{
  return backup.step(thd);
}

int innodb_backup_end(THD *thd, bool abort) noexcept
{
  return backup.end(thd, abort);
}

void innodb_backup_finalize(THD *thd) noexcept
{
  backup.fini(thd);
}
