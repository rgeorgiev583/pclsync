/* Copyright (c) 2013 Anton Titov.
 * Copyright (c) 2013 pCloud Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of pCloud Ltd nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL pCloud Ltd BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "plibs.h"
#include "pcompat.h"
#include "psynclib.h"
#include "pcallbacks.h"
#include "pstatus.h"
#include "pdiff.h"
#include "pssl.h"
#include "ptimer.h"
#include "pupload.h"
#include "pdownload.h"
#include "pfolder.h"
#include "psettings.h"
#include "psyncer.h"
#include "ptasks.h"
#include "papi.h"
#include "pnetlibs.h"
#include "pscanner.h"
#include "plocalscan.h"
#include "plist.h"
#include "pp2p.h"
#include "plocalnotify.h"
#include "pcache.h"
#include "pfileops.h"
#include "pcloudcrypto.h"
#include "ppagecache.h"
#include "ppassword.h"
#include <string.h>
#include <ctype.h>
#include <stddef.h>

typedef struct {
  psync_list list;
  char str[];
} string_list;

#if IS_DEBUG

static psync_malloc_t psync_real_malloc=malloc;

static void *debug_malloc(size_t sz){
  void *ptr;
  if (unlikely(sz>=PSYNC_DEBUG_LOG_ALLOC_OVER))
    debug(D_WARNING, "allocating %lu bytes", (unsigned long)sz);
  assert(sz>0);
  ptr=psync_real_malloc(sz);
  if (likely_log(ptr))
    memset(ptr, 0xfa, sz);
  return ptr;
}

psync_malloc_t psync_malloc=debug_malloc;

#else
psync_malloc_t psync_malloc=malloc;
#endif

psync_realloc_t psync_realloc=realloc;
psync_free_t psync_free=free;

const char *psync_database=NULL;

static int psync_libstate=0;
static pthread_mutex_t psync_libstate_mutex=PTHREAD_MUTEX_INITIALIZER;

#define return_error(err) do {psync_error=err; return -1;} while (0)
#define return_isyncid(err) do {psync_error=err; return PSYNC_INVALID_SYNCID;} while (0)

uint32_t psync_get_last_error(){
  return psync_error;
}

void psync_set_database_path(const char *databasepath){
  psync_database=psync_strdup(databasepath);
}

void psync_set_alloc(psync_malloc_t malloc_call, psync_realloc_t realloc_call, psync_free_t free_call){
#if IS_DEBUG
  psync_real_malloc=malloc_call;
#else
  psync_malloc=malloc_call;
#endif
  psync_realloc=realloc_call;
  psync_free=free_call;
}

static void psync_stop_crypto_on_sleep(){
  if (psync_setting_get_bool(_PS(sleepstopcrypto)) && psync_crypto_isstarted()){
    psync_cloud_crypto_stop();
    debug(D_NOTICE, "stopped crypto due to sleep");
  }
}

int psync_init(){
  psync_thread_name="main app thread";
  if (IS_DEBUG){
    pthread_mutex_lock(&psync_libstate_mutex);
    if (psync_libstate!=0){
      pthread_mutex_unlock(&psync_libstate_mutex);
      debug(D_BUG, "you are not supposed to call psync_init for a second time");
      return 0;
    }
  }
  psync_cache_init();
  psync_timer_init();
  psync_compat_init();
  if (!psync_database){
    psync_database=psync_get_default_database_path();
    if (unlikely_log(!psync_database)){
      if (IS_DEBUG)
        pthread_mutex_unlock(&psync_libstate_mutex);
      return_error(PERROR_NO_HOMEDIR);
    }
  }
  if (psync_sql_connect(psync_database)){
    if (IS_DEBUG)
      pthread_mutex_unlock(&psync_libstate_mutex);
    return_error(PERROR_DATABASE_OPEN);
  }
  psync_sql_statement("UPDATE task SET inprogress=0 WHERE inprogress=1");
  if (unlikely_log(psync_ssl_init())){
    if (IS_DEBUG)
      pthread_mutex_unlock(&psync_libstate_mutex);
    return_error(PERROR_SSL_INIT_FAILED);
  }
  psync_libs_init();
  psync_settings_init();
  psync_status_init();
  psync_timer_sleep_handler(psync_stop_crypto_on_sleep);
  if (IS_DEBUG){
    psync_libstate=1;
    pthread_mutex_unlock(&psync_libstate_mutex);
  }
  return 0;
}

void psync_start_sync(pstatus_change_callback_t status_callback, pevent_callback_t event_callback){
  if (IS_DEBUG){
    pthread_mutex_lock(&psync_libstate_mutex);
    if (psync_libstate==0){
      pthread_mutex_unlock(&psync_libstate_mutex);
      debug(D_BUG, "you are calling psync_start_sync before psync_init");
      return;
    }
    else if (psync_libstate==2){
      pthread_mutex_unlock(&psync_libstate_mutex);
      debug(D_BUG, "you are calling psync_start_sync for a second time");
      return;
    }
    else
      psync_libstate=2;
    pthread_mutex_unlock(&psync_libstate_mutex);
  }
  if (status_callback)
    psync_set_status_callback(status_callback);
  if (event_callback)
    psync_set_event_callback(event_callback);
  psync_syncer_init();
  psync_diff_init();
  psync_upload_init();
  psync_download_init();
  psync_netlibs_init();
  psync_localscan_init();
  psync_p2p_init();
  if (psync_setting_get_bool(_PS(autostartfs)))
    psync_fs_start();
}

uint32_t psync_download_state(){
  return 0;
}

void psync_destroy(){
  psync_do_run=0;
  psync_fs_stop();
  psync_send_status_update();
  psync_timer_wake();
  psync_timer_notify_exception();
  psync_sql_sync();
  psync_milisleep(20);
  psync_sql_lock();
  psync_cache_clean_all();
  psync_sql_close();
}

void psync_get_status(pstatus_t *status){
  psync_callbacks_get_status(status);
}

char *psync_get_username(){
  return psync_sql_cellstr("SELECT value FROM setting WHERE id='username'");
}

static void clear_db(int save){
  psync_sql_statement("DELETE FROM setting WHERE id IN ('pass', 'auth')");
  psync_setting_set_bool(_PS(saveauth), save);
}

void psync_set_user_pass(const char *username, const char *password, int save){
  clear_db(save);
  if (save){
    psync_set_string_value("user", username);
    psync_set_string_value("pass", password);
  }
  else{
    pthread_mutex_lock(&psync_my_auth_mutex);
    psync_free(psync_my_user);
    psync_my_user=psync_strdup(username);
    psync_free(psync_my_pass);
    psync_my_pass=psync_strdup(password);
    pthread_mutex_unlock(&psync_my_auth_mutex);
  }
  psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
}

void psync_set_pass(const char *password, int save){
  clear_db(save);
  if (save)
    psync_set_string_value("pass", password);
  else{
    pthread_mutex_lock(&psync_my_auth_mutex);
    psync_free(psync_my_pass);
    psync_my_pass=psync_strdup(password);
    pthread_mutex_unlock(&psync_my_auth_mutex);
  }
  psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
}

void psync_set_auth(const char *auth, int save){
  clear_db(save);
  if (save)
    psync_set_string_value("auth", auth);
  else
    strcpy(psync_my_auth, auth);
  psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
}

#define run_command(cmd, params, err) do_run_command_res(cmd, strlen(cmd), params, sizeof(params)/sizeof(binparam), err)

static int do_run_command_res(const char *cmd, size_t cmdlen, const binparam *params, size_t paramscnt, char **err){
  psync_socket *api;
  binresult *res;
  uint64_t result;
  api=psync_apipool_get();
  if (unlikely(!api))
    goto neterr;
  res=do_send_command(api, cmd, cmdlen, params, paramscnt, -1, 1);
  if (likely(res))
    psync_apipool_release(api);
  else{
    psync_apipool_release_bad(api);
    goto neterr;
  }
  result=psync_find_result(res, "result", PARAM_NUM)->num;
  if (result){
    debug(D_WARNING, "command %s returned code %u", cmd, (unsigned)result);
    if (err)
      *err=psync_strdup(psync_find_result(res, "error", PARAM_STR)->str);
  }
  psync_free(res);
  return (int)result;
neterr:
  if (err)
    *err=psync_strdup("Could not connect to the server.");
  return -1;
}

static void psync_invalidate_auth(const char *auth){
  binparam params[]={P_STR("auth", psync_my_auth)};
  run_command("logout", params, NULL);
}

void psync_logout(){
  debug(D_NOTICE, "logout");
  psync_sql_statement("DELETE FROM setting WHERE id IN ('pass', 'auth', 'saveauth')");
  psync_invalidate_auth(psync_my_auth);
  memset(psync_my_auth, 0, sizeof(psync_my_auth));
  psync_cloud_crypto_stop();
  pthread_mutex_lock(&psync_my_auth_mutex);
  psync_free(psync_my_pass);
  psync_my_pass=NULL;
  pthread_mutex_unlock(&psync_my_auth_mutex);
  psync_set_status(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_CONNECTING);
  psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_REQUIRED);
  psync_fs_pause_until_login();
  psync_stop_all_download();
  psync_stop_all_upload();
  psync_cache_clean_all();
  psync_restart_localscan();
  psync_timer_notify_exception();
  if (psync_fs_need_per_folder_refresh())
    psync_fs_refresh_folder(0);
}

void psync_unlink(){
  int ret;
  debug(D_NOTICE, "unlink");
  psync_diff_lock();
  psync_stop_all_download();
  psync_stop_all_upload();
  psync_status_recalc_to_download();
  psync_status_recalc_to_upload();
  psync_invalidate_auth(psync_my_auth);
  psync_cloud_crypto_stop();
  psync_milisleep(20);
  psync_stop_localscan();
  psync_sql_checkpoint_lock();
  psync_set_status(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_CONNECTING);
  psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_REQUIRED);
  psync_set_status(PSTATUS_TYPE_RUN, PSTATUS_RUN_STOP);
  psync_timer_notify_exception();
  psync_sql_lock();
  debug(D_NOTICE, "clearing database, locked");
  psync_cache_clean_all();
  ret=psync_sql_close();
  psync_file_delete(psync_database);
  if (ret){
    debug(D_ERROR, "failed to close database, exiting");
    exit(1);
  }
  psync_pagecache_clean_cache();
  psync_sql_connect(psync_database);
  /*
    psync_sql_res *res;
    psync_variant_row row;
    char *sql;
    const char *str;
    size_t len;
    psync_list list;
    string_list *le;
    psync_list_init(&list);
    res=psync_sql_query("SELECT name FROM sqlite_master WHERE type='index'");
    while ((row=psync_sql_fetch_row(res))){
      str=psync_get_lstring(row[0], &len);
      le=(string_list *)psync_malloc(offsetof(string_list, str)+len+1);
      memcpy(le->str, str, len+1);
      psync_list_add_tail(&list, &le->list);
    }
    psync_sql_free_result(res);
    psync_list_for_each_element(le, &list, string_list, list){
      sql=psync_strcat("DROP INDEX ", le->str, NULL);
      psync_sql_statement(sql);
      psync_free(sql);
    }
    psync_list_for_each_element_call(&list, string_list, list, psync_free);
    psync_list_init(&list);
    res=psync_sql_query("SELECT name FROM sqlite_master WHERE type='table'");
    while ((row=psync_sql_fetch_row(res))){
      str=psync_get_lstring(row[0], &len);
      le=(string_list *)psync_malloc(offsetof(string_list, str)+len+1);
      memcpy(le->str, str, len+1);
      psync_list_add_tail(&list, &le->list);
    }
    psync_sql_free_result(res);
    psync_list_for_each_element(le, &list, string_list, list){
      sql=psync_strcat("DROP TABLE ", le->str, NULL);
      psync_sql_statement(sql);
      psync_free(sql);
    }
    psync_list_for_each_element_call(&list, string_list, list, psync_free);
    psync_sql_statement("VACUUM");
  */
  pthread_mutex_lock(&psync_my_auth_mutex);
  memset(psync_my_auth, 0, sizeof(psync_my_auth));
  psync_my_user=NULL;
  psync_my_pass=NULL;
  psync_my_userid=0;
  pthread_mutex_unlock(&psync_my_auth_mutex);
  debug(D_NOTICE, "clearing database, finished");
  psync_fs_pause_until_login();
  psync_fs_clean_tasks();
  psync_sql_unlock();
  psync_sql_checkpoint_unlock();
  psync_settings_reset();
  psync_cache_clean_all();
  psync_diff_unlock();
  psync_set_status(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_CONNECTING);
  psync_set_status(PSTATUS_TYPE_ACCFULL, PSTATUS_ACCFULL_QUOTAOK);
  psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_REQUIRED);
  psync_set_status(PSTATUS_TYPE_RUN, PSTATUS_RUN_RUN);
  psync_resume_localscan();
  if (psync_fs_need_per_folder_refresh())
    psync_fs_refresh_folder(0);
}

psync_syncid_t psync_add_sync_by_path(const char *localpath, const char *remotepath, psync_synctype_t synctype){
  psync_folderid_t folderid=psync_get_folderid_by_path(remotepath);
  if (likely_log(folderid!=PSYNC_INVALID_FOLDERID))
    return psync_add_sync_by_folderid(localpath, folderid, synctype);
  else
    return PSYNC_INVALID_SYNCID;
}

psync_syncid_t psync_add_sync_by_folderid(const char *localpath, psync_folderid_t folderid, psync_synctype_t synctype){
  psync_sql_res *res;
  char *syncmp;
  psync_uint_row row;
  psync_str_row srow;
  uint64_t perms;
  psync_stat_t st;
  psync_syncid_t ret;
  int unsigned md;
  if (unlikely_log(synctype<PSYNC_SYNCTYPE_MIN || synctype>PSYNC_SYNCTYPE_MAX))
    return_isyncid(PERROR_INVALID_SYNCTYPE);
  if (unlikely_log(psync_stat(localpath, &st)) || unlikely_log(!psync_stat_isfolder(&st)))
    return_isyncid(PERROR_LOCAL_FOLDER_NOT_FOUND);
  if (synctype&PSYNC_DOWNLOAD_ONLY)
    md=7;
  else
    md=5;
  if (unlikely_log(!psync_stat_mode_ok(&st, md)))
    return_isyncid(PERROR_LOCAL_FOLDER_ACC_DENIED);
  syncmp=psync_fs_getmountpoint();
  if (syncmp){
    size_t len=strlen(syncmp);
    if (!psync_filename_cmpn(syncmp, localpath, len) && (localpath[len]==0 || localpath[len]=='/' || localpath[len]=='\\')){
      debug(D_NOTICE, "local path %s is on pCloudDrive mounted as %s, rejecting sync", localpath, syncmp);
      psync_free(syncmp);
      return_isyncid(PERROR_LOCAL_IS_ON_PDRIVE);
    }
    psync_free(syncmp);
  }
  res=psync_sql_query("SELECT localpath FROM syncfolder");
  if (unlikely_log(!res))
    return_isyncid(PERROR_DATABASE_ERROR);
  while ((srow=psync_sql_fetch_rowstr(res)))
    if (psync_str_is_prefix(srow[0], localpath)){
      psync_sql_free_result(res);
      return_isyncid(PERROR_PARENT_OR_SUBFOLDER_ALREADY_SYNCING);
    }
    else if (!psync_filename_cmp(srow[0], localpath)){
      psync_sql_free_result(res);
      return_isyncid(PERROR_FOLDER_ALREADY_SYNCING);
    }
  psync_sql_free_result(res);
  if (folderid){
    res=psync_sql_query("SELECT permissions FROM folder WHERE id=?");
    if (unlikely_log(!res))
      return_isyncid(PERROR_DATABASE_ERROR);
    psync_sql_bind_uint(res, 1, folderid);
    row=psync_sql_fetch_rowint(res);
    if (unlikely_log(!row)){
      psync_sql_free_result(res);
      return_isyncid(PERROR_REMOTE_FOLDER_NOT_FOUND);
    }
    perms=row[0];
    psync_sql_free_result(res);
  }
  else
    perms=PSYNC_PERM_ALL;
  if (unlikely_log((synctype&PSYNC_DOWNLOAD_ONLY && (perms&PSYNC_PERM_READ)!=PSYNC_PERM_READ) ||
      (synctype&PSYNC_UPLOAD_ONLY && (perms&PSYNC_PERM_WRITE)!=PSYNC_PERM_WRITE)))
    return_isyncid(PERROR_REMOTE_FOLDER_ACC_DENIED);
  res=psync_sql_prep_statement("INSERT OR IGNORE INTO syncfolder (folderid, localpath, synctype, flags, inode, deviceid) VALUES (?, ?, ?, 0, ?, ?)");
  if (unlikely_log(!res))
    return_isyncid(PERROR_DATABASE_ERROR);
  psync_sql_bind_uint(res, 1, folderid);
  psync_sql_bind_string(res, 2, localpath);
  psync_sql_bind_uint(res, 3, synctype);
  psync_sql_bind_uint(res, 4, psync_stat_inode(&st));
  psync_sql_bind_uint(res, 5, psync_stat_device(&st));
  psync_sql_run(res);
  if (likely_log(psync_sql_affected_rows()))
    ret=psync_sql_insertid();
  else
    ret=PSYNC_INVALID_SYNCID;
  psync_sql_free_result(res);
  if (ret==PSYNC_INVALID_SYNCID)
    return_isyncid(PERROR_FOLDER_ALREADY_SYNCING);
  psync_sql_sync();
  psync_syncer_new(ret);
  return ret;
}

int psync_add_sync_by_path_delayed(const char *localpath, const char *remotepath, psync_synctype_t synctype){
  psync_sql_res *res;
  psync_stat_t st;
  int unsigned md;
  if (unlikely_log(synctype<PSYNC_SYNCTYPE_MIN || synctype>PSYNC_SYNCTYPE_MAX))
    return_error(PERROR_INVALID_SYNCTYPE);
  if (unlikely_log(psync_stat(localpath, &st)) || unlikely_log(!psync_stat_isfolder(&st)))
    return_error(PERROR_LOCAL_FOLDER_NOT_FOUND);
  if (synctype&PSYNC_DOWNLOAD_ONLY)
    md=7;
  else
    md=5;
  if (unlikely_log(!psync_stat_mode_ok(&st, md)))
    return_error(PERROR_LOCAL_FOLDER_ACC_DENIED);
  res=psync_sql_prep_statement("INSERT INTO syncfolderdelayed (localpath, remotepath, synctype) VALUES (?, ?, ?)");
  psync_sql_bind_string(res, 1, localpath);
  psync_sql_bind_string(res, 2, remotepath);
  psync_sql_bind_uint(res, 3, synctype);
  psync_sql_run_free(res);
  psync_sql_sync();
  if (psync_status_get(PSTATUS_TYPE_ONLINE)==PSTATUS_ONLINE_ONLINE)
    psync_run_thread("check delayed syncs", psync_syncer_check_delayed_syncs);
  return 0;
}

int psync_change_synctype(psync_syncid_t syncid, psync_synctype_t synctype){
  psync_sql_res *res;
  psync_variant_row row;
  psync_uint_row urow;
  psync_folderid_t folderid;
  uint64_t perms;
  psync_stat_t st;
  int unsigned md;
  psync_synctype_t oldsynctype;
  if (unlikely_log(synctype<PSYNC_SYNCTYPE_MIN || synctype>PSYNC_SYNCTYPE_MAX))
    return_isyncid(PERROR_INVALID_SYNCTYPE);
  psync_sql_start_transaction();
  res=psync_sql_query("SELECT folderid, localpath, synctype FROM syncfolder WHERE id=?");
  psync_sql_bind_uint(res, 1, syncid);
  row=psync_sql_fetch_row(res);
  if (unlikely_log(!row)){
    psync_sql_free_result(res);
    psync_sql_rollback_transaction();
    return_error(PERROR_INVALID_SYNCID);
  }
  folderid=psync_get_number(row[0]);
  oldsynctype=psync_get_number(row[2]);
  if (oldsynctype==synctype){
    psync_sql_free_result(res);
    psync_sql_rollback_transaction();
    return 0;
  }
  if (unlikely_log(psync_stat(psync_get_string(row[1]), &st)) || unlikely_log(!psync_stat_isfolder(&st))){
    psync_sql_free_result(res);
    psync_sql_rollback_transaction();
    return_isyncid(PERROR_LOCAL_FOLDER_NOT_FOUND);
  }
  psync_sql_free_result(res);
  if (synctype&PSYNC_DOWNLOAD_ONLY)
    md=7;
  else
    md=5;
  if (unlikely_log(!psync_stat_mode_ok(&st, md))){
    psync_sql_rollback_transaction();
    return_isyncid(PERROR_LOCAL_FOLDER_ACC_DENIED);
  }
  if (folderid){
    res=psync_sql_query("SELECT permissions FROM folder WHERE id=?");
    if (unlikely_log(!res))
      return_isyncid(PERROR_DATABASE_ERROR);
    psync_sql_bind_uint(res, 1, folderid);
    urow=psync_sql_fetch_rowint(res);
    if (unlikely_log(!urow)){
      psync_sql_free_result(res);
      psync_sql_rollback_transaction();
      return_isyncid(PERROR_REMOTE_FOLDER_NOT_FOUND);
    }
    perms=urow[0];
    psync_sql_free_result(res);
  }
  else
    perms=PSYNC_PERM_ALL;
  if (unlikely_log((synctype&PSYNC_DOWNLOAD_ONLY && (perms&PSYNC_PERM_READ)!=PSYNC_PERM_READ) ||
      (synctype&PSYNC_UPLOAD_ONLY && (perms&PSYNC_PERM_WRITE)!=PSYNC_PERM_WRITE))){
    psync_sql_rollback_transaction();
    return_isyncid(PERROR_REMOTE_FOLDER_ACC_DENIED);
  }
  res=psync_sql_prep_statement("UPDATE syncfolder SET synctype=?, flags=0 WHERE id=?");
  psync_sql_bind_uint(res, 1, synctype);
  psync_sql_bind_uint(res, 2, syncid);
  psync_sql_run_free(res);
  res=psync_sql_query("SELECT folderid FROM syncedfolder WHERE syncid=?");
  psync_sql_bind_uint(res, 1, syncid);
  while ((urow=psync_sql_fetch_rowint(res)))
    psync_del_folder_from_downloadlist(urow[0]);
  psync_sql_free_result(res);
  res=psync_sql_prep_statement("DELETE FROM syncedfolder WHERE syncid=?");
  psync_sql_bind_uint(res, 1, syncid);
  psync_sql_run_free(res);
  res=psync_sql_prep_statement("DELETE FROM localfile WHERE syncid=?");
  psync_sql_bind_uint(res, 1, syncid);
  psync_sql_run_free(res);
  res=psync_sql_prep_statement("DELETE FROM localfolder WHERE syncid=?");
  psync_sql_bind_uint(res, 1, syncid);
  psync_sql_run_free(res);
  psync_sql_commit_transaction();
  psync_localnotify_del_sync(syncid);
  psync_stop_sync_download(syncid);
  psync_stop_sync_upload(syncid);
  psync_sql_sync();
  psync_syncer_new(syncid);
  return 0;
}

static void psync_delete_local_recursive(psync_syncid_t syncid, psync_folderid_t localfolderid){
  psync_sql_res *res;
  psync_uint_row row;
  res=psync_sql_query("SELECT id FROM localfolder WHERE localparentfolderid=? AND syncid=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  psync_sql_bind_uint(res, 2, syncid);
  while ((row=psync_sql_fetch_rowint(res)))
    psync_delete_local_recursive(syncid, row[0]);  
  psync_sql_free_result(res);
  res=psync_sql_prep_statement("DELETE FROM localfile WHERE localparentfolderid=? AND syncid=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  psync_sql_bind_uint(res, 2, syncid);
  psync_sql_run_free(res);
  res=psync_sql_prep_statement("DELETE FROM syncedfolder WHERE localfolderid=? AND syncid=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  psync_sql_bind_uint(res, 2, syncid);
  psync_sql_run_free(res);
  res=psync_sql_prep_statement("DELETE FROM localfolder WHERE id=? AND syncid=?");
  psync_sql_bind_uint(res, 1, localfolderid);
  psync_sql_bind_uint(res, 2, syncid);
  psync_sql_run_free(res);
}

int psync_delete_sync(psync_syncid_t syncid){
  psync_sql_res *res;
  psync_sql_start_transaction();
/* this is slow and unneeded:
  psync_uint_row row;
  res=psync_sql_query("SELECT type, itemid, localitemid FROM task WHERE syncid=?");
  psync_sql_bind_uint(res, 1, syncid);
  while ((row=psync_sql_fetch_rowint(res)))
    if (row[0]==PSYNC_DOWNLOAD_FILE)
      psync_stop_file_download(row[1], syncid);
    else if (row[0]==PSYNC_UPLOAD_FILE)
      psync_delete_upload_tasks_for_file(row[2]);
  psync_sql_free_result(res);
  */
  psync_delete_local_recursive(syncid, 0);
  res=psync_sql_prep_statement("DELETE FROM syncfolder WHERE id=?");
  psync_sql_bind_uint(res, 1, syncid);
  psync_sql_run_free(res);
  if (psync_sql_commit_transaction())
    return -1;
  else{
    psync_stop_sync_download(syncid);
    psync_stop_sync_upload(syncid);
    psync_localnotify_del_sync(syncid);
    psync_restart_localscan();
    psync_sql_sync();
    return 0;
  }
}

psync_folder_list_t *psync_get_sync_list(){
  return psync_list_get_list();
}

psuggested_folders_t *psync_get_sync_suggestions(){
  char *home;
  psuggested_folders_t *ret;
  home=psync_get_home_dir();
  if (likely_log(home)){
    ret=psync_scanner_scan_folder(home);
    psync_free(home);
    return ret;
  }
  else{
    psync_error=PERROR_NO_HOMEDIR;
    return NULL;
  }
}

pfolder_list_t *psync_list_local_folder_by_path(const char *localpath, psync_listtype_t listtype){
  return psync_list_local_folder(localpath, listtype);
}

pfolder_list_t *psync_list_remote_folder_by_path(const char *remotepath, psync_listtype_t listtype){
  psync_folderid_t folderid=psync_get_folderid_by_path(remotepath);
  if (folderid!=PSYNC_INVALID_FOLDERID)
    return psync_list_remote_folder(folderid, listtype);
  else
    return NULL;
}

pfolder_list_t *psync_list_remote_folder_by_folderid(psync_folderid_t folderid, psync_listtype_t listtype){
  return psync_list_remote_folder(folderid, listtype);
}

pentry_t *psync_stat_path(const char *remotepath){
  return psync_folder_stat_path(remotepath);
}

int psync_is_name_to_ignore(const char *name){
  const char *ign, *sc, *pt;
  char *namelower;
  unsigned char *lp;
  size_t ilen, off, pl;
  namelower=psync_strdup(name);
  lp=(unsigned char *)namelower;
  while (*lp){
    *lp=tolower(*lp);
    lp++;
  }
  ign=psync_setting_get_string(_PS(ignorepatterns));
  ilen=strlen(ign);
  off=0;
  do {
    sc=(const char *)memchr(ign+off, ';', ilen-off);
    if (sc)
      pl=sc-ign-off;
    else
      pl=ilen-off;
    pt=ign+off;
    off+=pl+1;
    while (pl && isspace((unsigned char)*pt)){
      pt++;
      pl--;
    }
    while (pl && isspace((unsigned char)pt[pl-1]))
      pl--;
    if (psync_match_pattern(namelower, pt, pl)){
      psync_free(namelower);
      debug(D_NOTICE, "ignoring file/folder %s", name);
      return 1;
    }
  } while (sc);
  psync_free(namelower);
  return 0;
}

static void psync_set_run_status(uint32_t status){
  psync_set_status(PSTATUS_TYPE_RUN, status);
  psync_set_uint_value("runstatus", status);
}

int psync_pause(){
  psync_set_run_status(PSTATUS_RUN_PAUSE);
  return 0;
}

int psync_stop(){
  psync_set_run_status(PSTATUS_RUN_STOP);
  psync_timer_notify_exception();
  return 0;
}

int psync_resume(){
  psync_set_run_status(PSTATUS_RUN_RUN);
  return 0;
}

void psync_run_localscan(){
  psync_wake_localscan();
}

#define run_command_get_res(cmd, params, err, res) do_run_command_get_res(cmd, strlen(cmd), params, sizeof(params)/sizeof(binparam), err, res)

static int do_run_command_get_res(const char *cmd, size_t cmdlen, const binparam *params, size_t paramscnt, char **err, binresult **pres){
  psync_socket *api;
  binresult *res;
  uint64_t result;
  api=psync_apipool_get();
  if (unlikely(!api))
    goto neterr;
  res=do_send_command(api, cmd, cmdlen, params, paramscnt, -1, 1);
  if (likely(res))
    psync_apipool_release(api);
  else{
    psync_apipool_release_bad(api);
    goto neterr;
  }
  result=psync_find_result(res, "result", PARAM_NUM)->num;
  if (result){
    debug(D_WARNING, "command %s returned code %u", cmd, (unsigned)result);
    if (err)
      *err=psync_strdup(psync_find_result(res, "error", PARAM_STR)->str);
  }
  if (result)
    psync_free(res);
  else
    *pres=res;
  return (int)result;
neterr:
  if (err)
    *err=psync_strdup("Could not connect to the server.");
  return -1;
}

int psync_register(const char *email, const char *password, int termsaccepted, char **err){
  binparam params[]={P_STR("mail", email), P_STR("password", password), P_STR("termsaccepted", termsaccepted?"yes":"0"), P_NUM("os", P_OS_ID)};
  return run_command("register", params, err);
}

int psync_verify_email(char **err){
  binparam params[]={P_STR("auth", psync_my_auth)};
  return run_command("sendverificationemail", params, err);
}

int psync_lost_password(const char *email, char **err){
  binparam params[]={P_STR("mail", email)};
  return run_command("lostpassword", params, err);
}

int psync_change_password(const char *currentpass, const char *newpass, char **err){
  binparam params[]={P_STR("auth", psync_my_auth), P_STR("oldpassword", currentpass), P_STR("newpassword", newpass)};
  return run_command("changepassword", params, err);
}

int psync_create_remote_folder_by_path(const char *path, char **err){
  binparam params[]={P_STR("auth", psync_my_auth), P_STR("path", path), P_STR("timeformat", "timestamp")};
  binresult *res;
  int ret;
  ret=run_command_get_res("createfolder", params, err, &res);
  if (ret)
    return ret;
  psync_ops_create_folder_in_db(psync_find_result(res, "metadata", PARAM_HASH));
  psync_free(res);
  return 0;
}

int psync_create_remote_folder(psync_folderid_t parentfolderid, const char *name, char **err){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("folderid", parentfolderid), P_STR("name", name), P_STR("timeformat", "timestamp")};
  binresult *res;
  int ret;
  ret=run_command_get_res("createfolder", params, err, &res);
  if (ret)
    return ret;
  psync_ops_create_folder_in_db(psync_find_result(res, "metadata", PARAM_HASH));
  psync_free(res);
  return 0;
}

const char *psync_get_auth_string(){
  return psync_my_auth;
}

int psync_get_bool_setting(const char *settingname){
  return psync_setting_get_bool(psync_setting_getid(settingname));
}

int psync_set_bool_setting(const char *settingname, int value){
  return psync_setting_set_bool(psync_setting_getid(settingname), value);
}

int64_t psync_get_int_setting(const char *settingname){
  return psync_setting_get_int(psync_setting_getid(settingname));
}

int psync_set_int_setting(const char *settingname, int64_t value){
  return psync_setting_set_int(psync_setting_getid(settingname), value);
}

uint64_t psync_get_uint_setting(const char *settingname){
  return psync_setting_get_uint(psync_setting_getid(settingname));
}

int psync_set_uint_setting(const char *settingname, uint64_t value){
  return psync_setting_set_uint(psync_setting_getid(settingname), value);
}

const char *psync_get_string_setting(const char *settingname){
  return psync_setting_get_string(psync_setting_getid(settingname));
}

int psync_set_string_setting(const char *settingname, const char *value){
  return psync_setting_set_string(psync_setting_getid(settingname), value);
}

int psync_has_value(const char *valuename){
  psync_sql_res *res;
  psync_uint_row row;
  int ret;
  res=psync_sql_query_rdlock("SELECT COUNT(*) FROM setting WHERE id=?");
  psync_sql_bind_string(res, 1, valuename);
  row=psync_sql_fetch_rowint(res);
  if (row)
    ret=row[0];
  else
    ret=0;
  psync_sql_free_result(res);
  return ret;
}

int psync_get_bool_value(const char *valuename){
  return !!psync_get_uint_value(valuename);
}

void psync_set_bool_value(const char *valuename, int value){
  psync_set_uint_value(valuename, (uint64_t)(!!value));
}

int64_t psync_get_int_value(const char *valuename){
  return (int64_t)psync_get_uint_value(valuename);
}

void psync_set_int_value(const char *valuename, int64_t value){
  psync_set_uint_value(valuename, (uint64_t)value);
}

uint64_t psync_get_uint_value(const char *valuename){
  psync_sql_res *res;
  psync_uint_row row;
  uint64_t ret;
  res=psync_sql_query_rdlock("SELECT value FROM setting WHERE id=?");
  psync_sql_bind_string(res, 1, valuename);
  row=psync_sql_fetch_rowint(res);
  if (row)
    ret=row[0];
  else
    ret=0;
  psync_sql_free_result(res);
  return ret;
}

void psync_set_uint_value(const char *valuename, uint64_t value){
  psync_sql_res *res;
  res=psync_sql_prep_statement("REPLACE INTO setting (id, value) VALUES (?, ?)");
  psync_sql_bind_string(res, 1, valuename);
  psync_sql_bind_uint(res, 2, value);
  psync_sql_run_free(res);
}

char *psync_get_string_value(const char *valuename){
  psync_sql_res *res;
  psync_str_row row;
  char *ret;
  res=psync_sql_query_rdlock("SELECT value FROM setting WHERE id=?");
  psync_sql_bind_string(res, 1, valuename);
  row=psync_sql_fetch_rowstr(res);
  if (row)
    ret=psync_strdup(row[0]);
  else
    ret=NULL;
  psync_sql_free_result(res);
  return ret;
}

void psync_set_string_value(const char *valuename, const char *value){
  psync_sql_res *res;
  res=psync_sql_prep_statement("REPLACE INTO setting (id, value) VALUES (?, ?)");
  psync_sql_bind_string(res, 1, valuename);
  psync_sql_bind_string(res, 2, value);
  psync_sql_run_free(res);
}

void psync_network_exception(){
  psync_timer_notify_exception();
}

static int create_request(psync_list_builder_t *builder, void *element, psync_variant_row row){
  psync_sharerequest_t *request;
  const char *str;
  uint32_t perms;
  size_t len;
  request=(psync_sharerequest_t *)element;
  request->sharerequestid=psync_get_number(row[0]);
  request->folderid=psync_get_number(row[1]);
  request->created=psync_get_number(row[2]);
  perms=psync_get_number(row[3]);
  request->userid=psync_get_number_or_null(row[4]);
  str=psync_get_lstring(row[5], &len);
  request->email=str;
  psync_list_add_lstring_offset(builder, offsetof(psync_sharerequest_t, email), len);
  str=psync_get_lstring(row[6], &len);
  request->sharename=str;
  psync_list_add_lstring_offset(builder, offsetof(psync_sharerequest_t, sharename), len);
  str=psync_get_lstring_or_null(row[7], &len);
  if (str){
    request->message=str;
    psync_list_add_lstring_offset(builder, offsetof(psync_sharerequest_t, message), len);
  }
  else{
    request->message="";
  }
  request->permissions=perms;
  request->canread=(perms&PSYNC_PERM_READ)/PSYNC_PERM_READ;
  request->cancreate=(perms&PSYNC_PERM_CREATE)/PSYNC_PERM_CREATE;
  request->canmodify=(perms&PSYNC_PERM_MODIFY)/PSYNC_PERM_MODIFY;
  request->candelete=(perms&PSYNC_PERM_DELETE)/PSYNC_PERM_DELETE;
  return 0;
}

psync_sharerequest_list_t *psync_list_sharerequests(int incoming){
  psync_list_builder_t *builder;
  psync_sql_res *res;
  builder=psync_list_builder_create(sizeof(psync_sharerequest_t), offsetof(psync_sharerequest_list_t, sharerequests));
  incoming=!!incoming;
  res=psync_sql_query_rdlock("SELECT id, folderid, ctime, permissions, userid, mail, name, message FROM sharerequest WHERE isincoming=? ORDER BY name");
  psync_sql_bind_uint(res, 1, incoming);
  psync_list_bulder_add_sql(builder, res, create_request);
  return (psync_sharerequest_list_t *)psync_list_builder_finalize(builder);
}

static int create_share(psync_list_builder_t *builder, void *element, psync_variant_row row){
  psync_share_t *share;
  const char *str;
  uint32_t perms;
  size_t len;
  share=(psync_share_t *)element;
  share->shareid=psync_get_number(row[0]);
  share->folderid=psync_get_number(row[1]);
  share->created=psync_get_number(row[2]);
  perms=psync_get_number(row[3]);
  share->userid=psync_get_number(row[4]);
  str=psync_get_lstring(row[5], &len);
  share->email=str;
  psync_list_add_lstring_offset(builder, offsetof(psync_share_t, email), len);
  str=psync_get_lstring(row[6], &len);
  share->sharename=str;
  psync_list_add_lstring_offset(builder, offsetof(psync_share_t, sharename), len);
  share->permissions=perms;
  share->canread=(perms&PSYNC_PERM_READ)/PSYNC_PERM_READ;
  share->cancreate=(perms&PSYNC_PERM_CREATE)/PSYNC_PERM_CREATE;
  share->canmodify=(perms&PSYNC_PERM_MODIFY)/PSYNC_PERM_MODIFY;
  share->candelete=(perms&PSYNC_PERM_DELETE)/PSYNC_PERM_DELETE;
  return 0;
}

psync_share_list_t *psync_list_shares(int incoming){
  psync_list_builder_t *builder;
  psync_sql_res *res;
  builder=psync_list_builder_create(sizeof(psync_share_t), offsetof(psync_share_list_t, shares));
  incoming=!!incoming;
  res=psync_sql_query_rdlock("SELECT id, folderid, ctime, permissions, userid, mail, name FROM sharedfolder WHERE isincoming=? ORDER BY name");
  psync_sql_bind_uint(res, 1, incoming);
  psync_list_bulder_add_sql(builder, res, create_share);
  return (psync_share_list_t *)psync_list_builder_finalize(builder);
}

static uint32_t convert_perms(uint32_t permissions){
  return 
    (permissions&PSYNC_PERM_CREATE)/PSYNC_PERM_CREATE*1+
    (permissions&PSYNC_PERM_MODIFY)/PSYNC_PERM_MODIFY*2+
    (permissions&PSYNC_PERM_DELETE)/PSYNC_PERM_DELETE*4;
}

int psync_share_folder(psync_folderid_t folderid, const char *name, const char *mail, const char *message, uint32_t permissions, char **err){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("folderid", folderid), P_STR("name", name), P_STR("mail", mail),
                     P_STR("message", message), P_NUM("permissions", convert_perms(permissions))};
  return run_command("sharefolder", params, err);
}

int psync_cancel_share_request(psync_sharerequestid_t requestid, char **err){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("sharerequestid", requestid)};
  return run_command("cancelsharerequest", params, err);
}

int psync_decline_share_request(psync_sharerequestid_t requestid, char **err){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("sharerequestid", requestid)};
  return run_command("declineshare", params, err);
}

int psync_accept_share_request(psync_sharerequestid_t requestid, psync_folderid_t tofolderid, const char *name, char **err){
  if (name){
    binparam params[]={P_STR("auth", psync_my_auth), P_NUM("sharerequestid", requestid), P_NUM("folderid", tofolderid), P_STR("name", name)};
    return run_command("acceptshare", params, err);
  }
  else{
    binparam params[]={P_STR("auth", psync_my_auth), P_NUM("sharerequestid", requestid), P_NUM("folderid", tofolderid)};
    return run_command("acceptshare", params, err);
  }
}

int psync_remove_share(psync_shareid_t shareid, char **err){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("shareid", shareid)};
  return run_command("removeshare", params, err);
}


int psync_modify_share(psync_shareid_t shareid, uint32_t permissions, char **err){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("shareid", shareid), P_NUM("permissions", convert_perms(permissions))};
  return run_command("changeshare", params, err);
}

static unsigned long psync_parse_version(const char *currentversion){
  unsigned long cv, cm;
  cv=cm=0;
  while (1){
    if (*currentversion=='.'){
      cv=(cv+cm)*100;
      cm=0;
    }
    else if (*currentversion==0)
      return cv+cm;
    else if (*currentversion>='0' && *currentversion<='9')
      cm=cm*10+*currentversion-'0';
    else
      debug(D_WARNING, "invalid characters in version string: %s", currentversion);
    currentversion++;
  }
}

psync_new_version_t *psync_check_new_version_str(const char *os, const char *currentversion){
  return psync_check_new_version(os, psync_parse_version(currentversion));
}

static psync_new_version_t *psync_res_to_ver(const binresult *res, char *localpath){
  psync_new_version_t *ver;
  const char *notes, *versionstr;
  size_t lurl, lnotes, lversion, llpath, llocalpath;
  const binresult *cres, *pres, *hres;
  char *ptr;
  unsigned long usize;
  cres=psync_find_result(res, "download", PARAM_HASH);
  lurl=sizeof("https://")-1;
  pres=psync_find_result(cres, "path", PARAM_STR);
  lurl+=pres->length;
  hres=psync_find_result(cres, "hosts", PARAM_ARRAY)->array[0];
  lurl+=hres->length;
  lurl=(lurl+sizeof(void *))/sizeof(void *)*sizeof(void *);
  usize=psync_find_result(cres, "size", PARAM_NUM)->num;
  cres=psync_find_result(res, "notes", PARAM_STR);
  notes=cres->str;
  lnotes=(cres->length+sizeof(void *))/sizeof(void *)*sizeof(void *);
  cres=psync_find_result(res, "versionstr", PARAM_STR);
  versionstr=cres->str;
  lversion=(cres->length+sizeof(void *))/sizeof(void *)*sizeof(void *);
  if (localpath){
    llpath=strlen(localpath);
    llocalpath=(llpath+sizeof(void *))/sizeof(void *)*sizeof(void *);
  }
  else
    llpath=llocalpath=0;
  ver=(psync_new_version_t *)psync_malloc(sizeof(psync_new_version_t)+lurl+lnotes+lversion+llocalpath);
  ptr=(char *)(ver+1);
  ver->url=ptr;
  memcpy(ptr, "https://", sizeof("https://")-1);
  ptr+=sizeof("https://")-1;
  memcpy(ptr, hres->str, hres->length);
  ptr+=hres->length;
  memcpy(ptr, pres->str, pres->length+1);
  ptr=(char *)ver->url+lurl;
  memcpy(ptr, notes, lnotes);
  ver->notes=ptr;
  ptr+=lnotes;
  memcpy(ptr, versionstr, lversion);
  ver->versionstr=ptr;
  if (localpath){
    ptr+=lversion;
    memcpy(ptr, localpath, llpath+1);
    ver->localpath=ptr;
  }
  else
    ver->localpath=NULL;
  ver->version=psync_find_result(res, "version", PARAM_NUM)->num;
  ver->updatesize=usize;
  return ver;
}

psync_new_version_t *psync_check_new_version(const char *os, unsigned long currentversion){
  binparam params[]={P_STR("os", os), P_NUM("version", currentversion)};
  psync_new_version_t *ver;
  binresult *res;
  int ret;
  ret=run_command_get_res("getlastversion", params, NULL, &res);
  if (ret){
    debug(D_WARNING, "getlastversion returned %d", ret);
    return NULL;
  }
  if (!psync_find_result(res, "newversion", PARAM_BOOL)->num){
    psync_free(res);
    return NULL;
  }
  ver=psync_res_to_ver(res, NULL);
  psync_free(res);
  return ver;
}

static void psync_del_all_except(void *ptr, psync_pstat_fast *st){
  const char **nmarr;
  char *fp;
  nmarr=(const char **)ptr;
  if (!psync_filename_cmp(st->name, nmarr[1]) || st->isfolder)
    return;
  fp=psync_strcat(nmarr[0], PSYNC_DIRECTORY_SEPARATOR, st->name, NULL);
  debug(D_NOTICE, "deleting old update file %s", fp);
  if (psync_file_delete(fp))
    debug(D_WARNING, "could not delete %s", fp);
  psync_free(fp);
}

static char *psync_filename_from_res(const binresult *res){
  const char *nm;
  char *nmd, *path, *ret;
  const char *nmarr[2];
  nm=strrchr(psync_find_result(res, "path", PARAM_STR)->str, '/');
  if (unlikely_log(!nm))
    return NULL;
  path=psync_get_private_tmp_dir();
  if (unlikely_log(!path))
    return NULL;
  nmd=psync_url_decode(nm+1);
  nmarr[0]=path;
  nmarr[1]=nmd;
  psync_list_dir_fast(path, psync_del_all_except, (void *)nmarr);
  ret=psync_strcat(path, PSYNC_DIRECTORY_SEPARATOR, nmd, NULL);
  psync_free(nmd);
  psync_free(path);
  return ret;
}

static int psync_download_new_version(const binresult *res, char **lpath){
  const char *host;
  psync_http_socket *sock;
  char *buff, *filename;
  uint64_t size;
  psync_stat_t st;
  psync_file_t fd;
  int rd;
  sock=psync_http_connect_multihost(psync_find_result(res, "hosts", PARAM_ARRAY), &host);
  if (unlikely_log(!sock))
    return -1;
  if (unlikely_log(psync_http_request(sock, host, psync_find_result(res, "path", PARAM_STR)->str, 0, 0))){
    psync_http_close(sock);
    return -1;
  }
  if (unlikely_log(psync_http_next_request(sock))){
    psync_http_close(sock);
    return 1;
  }
  size=psync_find_result(res, "size", PARAM_NUM)->num;
  filename=psync_filename_from_res(res);
  if (unlikely_log(!filename)){
    psync_http_close(sock);
    return 1;
  }
  if (!psync_stat(filename, &st) && psync_stat_size(&st)==size){
    *lpath=filename;
    psync_http_close(sock);
    return 0;
  }
  if (unlikely_log((fd=psync_file_open(filename, P_O_WRONLY, P_O_CREAT|P_O_TRUNC))==INVALID_HANDLE_VALUE)){
    psync_free(filename);
    psync_http_close(sock);
    return 1;
  }
  buff=(char *)psync_malloc(PSYNC_COPY_BUFFER_SIZE);
  while (size){
    rd=psync_http_request_readall(sock, buff, PSYNC_COPY_BUFFER_SIZE);
    if (unlikely_log(rd<=0 || psync_file_write(fd, buff, rd)!=rd))
      break;
    size-=rd;
  }
  psync_free(buff);
  psync_file_close(fd);
  psync_http_close(sock);
  if (unlikely_log(size)){
    psync_free(filename);
    return -1;
  }
  *lpath=filename;
  return 0;
}

psync_new_version_t *psync_check_new_version_download_str(const char *os, const char *currentversion){
  return psync_check_new_version_download(os, psync_parse_version(currentversion));
}

psync_new_version_t *psync_check_new_version_download(const char *os, unsigned long currentversion){
  binparam params[]={P_STR("os", os), P_NUM("version", currentversion)};
  psync_new_version_t *ver;
  binresult *res;
  char *lfilename;
  int ret;
  ret=run_command_get_res("getlastversion", params, NULL, &res);
  if (unlikely(ret==-1))
    do{
      debug(D_WARNING, "could not connect to server, sleeping");
      psync_milisleep(10000);
      ret=run_command_get_res("getlastversion", params, NULL, &res);
    } while (ret==-1);
  if (ret){
    debug(D_WARNING, "getlastversion returned %d", ret);
    return NULL;
  }
  if (!psync_find_result(res, "newversion", PARAM_BOOL)->num){
    psync_free(res);
    return NULL;
  }
  ret=psync_download_new_version(psync_find_result(res, "download", PARAM_HASH), &lfilename);
  if (unlikely(ret==-1))
    do{
      debug(D_WARNING, "could not download update, sleeping");
      psync_milisleep(10000);
      ret=psync_download_new_version(psync_find_result(res, "download", PARAM_HASH), &lfilename);
    } while (ret==-1);
  if (unlikely_log(ret)){
    psync_free(res);
    return NULL;
  }
  debug(D_NOTICE, "update downloaded to %s", lfilename);
  ver=psync_res_to_ver(res, lfilename);
  psync_free(lfilename);
  psync_free(res);
  return ver;
}

void psync_run_new_version(psync_new_version_t *ver){
  debug(D_NOTICE, "running %s", ver->localpath);
  if (psync_run_update_file(ver->localpath))
    return;
  psync_destroy();
  exit(0);
}

int psync_password_quality(const char *password){
  uint64_t score=psync_password_score(password);
  if (score<(uint64_t)1<<30)
    return 0;
  if (score<(uint64_t)1<<40)
    return 1;
  else
    return 2;
}

int psync_password_quality10000(const char *password){
  uint64_t score=psync_password_score(password);
  if (score<(uint64_t)1<<30)
    return score/(((uint64_t)1<<30)/10000+1);
  if (score<(uint64_t)1<<40)
    return (score-((uint64_t)1<<30))/((((uint64_t)1<<40)-((uint64_t)1<<30))/10000+1)+10000;
  else{
    if (score>=((uint64_t)1<<45)-((uint64_t)1<<40))
      return 29999;
    else
      return (score-((uint64_t)1<<40))/((((uint64_t)1<<45)-((uint64_t)1<<40))/10000+1)+20000;
  }
}

int psync_crypto_setup(const char *password, const char *hint){
  if (psync_status_is_offline())
    return PSYNC_CRYPTO_SETUP_CANT_CONNECT;
  else
    return psync_cloud_crypto_setup(password, hint);
}

int psync_crypto_get_hint(char **hint){
  if (psync_status_is_offline())
    return PSYNC_CRYPTO_HINT_CANT_CONNECT;
  else
    return psync_cloud_crypto_get_hint(hint);
}

int psync_crypto_start(const char *password){
  return psync_cloud_crypto_start(password);
}

int psync_crypto_stop(){
  return psync_cloud_crypto_stop();
}

int psync_crypto_isstarted(){
  return psync_cloud_crypto_isstarted();
}

int psync_crypto_mkdir(psync_folderid_t folderid, const char *name, const char **err, psync_folderid_t *newfolderid){
  if (psync_status_is_offline())
    return PSYNC_CRYPTO_CANT_CONNECT;
  else
    return psync_cloud_crypto_mkdir(folderid, name, err, newfolderid);
}

int psync_crypto_issetup(){
  return psync_sql_cellint("SELECT value FROM setting WHERE id='cryptosetup'", 0);
}

int psync_crypto_hassubscription(){
  return psync_sql_cellint("SELECT value FROM setting WHERE id='cryptosubscription'", 0);
}

int psync_crypto_isexpired(){
  int64_t ce;
  ce=psync_sql_cellint("SELECT value FROM setting WHERE id='cryptoexpires'", 0);
  return ce?(ce<psync_timer_time()):0;
}

time_t psync_crypto_expires(){
  return psync_sql_cellint("SELECT value FROM setting WHERE id='cryptoexpires'", 0);
}

int psync_crypto_reset(){
  if (psync_status_is_offline())
    return PSYNC_CRYPTO_RESET_CANT_CONNECT;
  else
    return psync_cloud_crypto_reset();
}

psync_folderid_t psync_crypto_folderid(){
  int64_t id;
  id=psync_sql_cellint("SELECT id FROM folder WHERE parentfolderid=0 AND flags&"NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED)"="NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED)" LIMIT 1", 0);
  if (id)
    return id;
  id=psync_sql_cellint("SELECT f1.id FROM folder f1, folder f2 WHERE f1.parentfolderid=f2.id AND "
                       "f1.flags&"NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED)"="NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED)" AND "
                       "f2.flags&"NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED)"=0 LIMIT 1", 0);
  if (id)
    return id;
  else
    return PSYNC_CRYPTO_INVALID_FOLDERID;
}

psync_folderid_t *psync_crypto_folderids(){
  psync_sql_res *res;
  psync_uint_row row;
  psync_folderid_t *ret;
  size_t alloc, l;
  alloc=2;
  l=0;
  ret=psync_new_cnt(psync_folderid_t, alloc);
  res=psync_sql_query_rdlock("SELECT f1.id FROM folder f1, folder f2 WHERE f1.parentfolderid=f2.id AND "
                             "f1.flags&"NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED)"="NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED)" AND "
                             "f2.flags&"NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED)"=0");
  while ((row=psync_sql_fetch_rowint(res))){
    ret[l]=row[0];
    if (++l==alloc){
      alloc*=2;
      ret=(psync_folderid_t *)psync_realloc(ret, sizeof(psync_folderid_t)*alloc);
    }
  }
  psync_sql_free_result(res);
  ret[l]=PSYNC_CRYPTO_INVALID_FOLDERID;
  return ret;
}

