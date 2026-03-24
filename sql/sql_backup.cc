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
#include "sql_backup.h"
#include "sql_parse.h"

static my_bool backup_start(THD *thd, plugin_ref plugin, void *target)
  noexcept
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->backup_start)
    return hton->backup_start(thd, static_cast<LEX_CSTRING*>(target));
  return false;
}

static my_bool backup_end(THD *thd, plugin_ref plugin, void *arg) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->backup_end)
    return hton->backup_end(thd, arg != nullptr);
  return false;
}

static my_bool backup_step(THD *thd, plugin_ref plugin, void *) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  int res= 0;
  if (hton->backup_step)
    while ((res= hton->backup_step(thd)))
      if (res < 0)
        break;
  return res != 0;
}

static my_bool backup_finalize(THD *thd, plugin_ref plugin, void *) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->backup_step)
    hton->backup_finalize(thd);
  return 0;
}

bool Sql_cmd_backup::execute(THD *thd)
{
  if (check_global_access(thd, RELOAD_ACL) ||
      check_global_access(thd, SELECT_ACL) ||
      error_if_data_home_dir(target.str, "BACKUP SERVER TO"))
    return true;

  if (my_mkdir(target.str, 0755, MYF(MY_WME)))
    return true;

  bool fail= plugin_foreach_with_mask(thd, backup_start,
                                      MYSQL_STORAGE_ENGINE_PLUGIN,
                                      PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                                      const_cast<LEX_CSTRING*>(&target));
  if (!fail)
    fail= plugin_foreach_with_mask(thd, backup_step,
                                   MYSQL_STORAGE_ENGINE_PLUGIN,
                                   PLUGIN_IS_DELETED|PLUGIN_IS_READY, nullptr);

  plugin_foreach_with_mask(thd, backup_end, MYSQL_STORAGE_ENGINE_PLUGIN,
                           PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                           reinterpret_cast<void*>(fail));

  plugin_foreach_with_mask(thd, backup_finalize, MYSQL_STORAGE_ENGINE_PLUGIN,
                           PLUGIN_IS_DELETED|PLUGIN_IS_READY, nullptr);

  my_ok(thd);
  return false;
}
