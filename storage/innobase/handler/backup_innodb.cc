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

  /** name of the target directory */
  const LEX_CSTRING *target;

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
  int init(THD *thd, const LEX_CSTRING *target) noexcept
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
      /* TODO: copy the file to target safely, even when there may be
      concurrent buf_page_t::flush() to this tablespace */
      sql_print_information("BACKUP SERVER: copy %s",
                            UT_LIST_GET_FIRST(space->chain)->name);
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
};

/** The backup context */
static InnoDB_backup backup;
}

int innodb_backup_start(THD *thd, const LEX_CSTRING *target) noexcept
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
