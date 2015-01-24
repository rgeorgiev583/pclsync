/* Copyright (c) 2013-2014 Anton Titov.
 * Copyright (c) 2013-2014 pCloud Ltd.
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

#ifndef _PSYNC_LIB_H
#define _PSYNC_LIB_H

/* All paths are in UTF-8 regardless of the OS.
 * All functions with int return type unless specified otherwise return 0 for success
 * and -1 for failure.
 */

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef uint64_t psync_folderid_t;
typedef uint64_t psync_fileid_t;
typedef uint64_t psync_fileorfolderid_t;
typedef uint64_t psync_userid_t;
typedef uint64_t psync_shareid_t;
typedef uint64_t psync_sharerequestid_t;
typedef uint32_t psync_syncid_t;
typedef uint32_t psync_eventtype_t;
typedef uint32_t psync_synctype_t;
typedef uint32_t psync_listtype_t;

typedef struct {
  psync_fileid_t fileid;
  uint64_t size;
} pfile_t;

typedef struct {
  psync_folderid_t folderid;
  uint8_t cansyncup;
  uint8_t cansyncdown;
  uint8_t canshare;
  uint8_t isencrypted;
} pfolder_t;

typedef struct {
  const char *name;
  union {
    pfolder_t folder;
    pfile_t file;
  };
  uint16_t namelen;
  uint8_t isfolder;
} pentry_t;

typedef struct {
  size_t entrycnt;
  pentry_t entries[];
} pfolder_list_t;

typedef struct {
  const char *localpath;
  const char *name;
  const char *description;
} psuggested_folder_t;

typedef struct {
  size_t entrycnt;
  psuggested_folder_t entries[];
} psuggested_folders_t;

#define PSTATUS_READY                   0
#define PSTATUS_DOWNLOADING             1
#define PSTATUS_UPLOADING               2
#define PSTATUS_DOWNLOADINGANDUPLOADING 3
#define PSTATUS_LOGIN_REQUIRED          4
#define PSTATUS_BAD_LOGIN_DATA          5
#define PSTATUS_BAD_LOGIN_TOKEN         6
#define PSTATUS_ACCOUNT_FULL            7
#define PSTATUS_DISK_FULL               8
#define PSTATUS_PAUSED                  9
#define PSTATUS_STOPPED                10
#define PSTATUS_OFFLINE                11
#define PSTATUS_CONNECTING             12
#define PSTATUS_SCANNING               13
#define PSTATUS_USER_MISMATCH          14

typedef struct {
  const char *downloadstr; /* formatted string with the status of uploads */
  const char *uploadstr;   /* formatted string with the status of downloads */
  uint64_t bytestoupload; /* sum of the sizes of files that need to be uploaded to sync state */
  uint64_t bytestouploadcurrent; /* sum of the sizes of files in filesuploading */
  uint64_t bytesuploaded; /* bytes uploaded in files accounted in filesuploading */
  uint64_t bytestodownload; /* sum of the sizes of files that need to be downloaded to sync state */
  uint64_t bytestodownloadcurrent; /* sum of the sizes of files in filesdownloading */
  uint64_t bytesdownloaded; /* bytes downloaded in files accounted in filesdownloading */
  uint32_t status; /* current status, one of PSTATUS_ constants */
  uint32_t filestoupload; /* number of files to upload in order to sync state, including filesuploading*/
  uint32_t filesuploading; /* number of files currently uploading */
  uint32_t uploadspeed; /* in bytes/sec */
  uint32_t filestodownload;  /* number of files to download in order to sync state, including filesdownloading*/
  uint32_t filesdownloading; /* number of files currently downloading */
  uint32_t downloadspeed; /* in bytes/sec */
  uint8_t remoteisfull; /* account is full and no files will be synced upwards*/
  uint8_t localisfull; /* (some) local hard drive is full and no files will be synced from the cloud */
} pstatus_t;

/* PEVENT_LOCAL_FOLDER_CREATED means that a folder was created in remotely and this action was replicated
 * locally, not the other way around. Accordingly PEVENT_REMOTE_FOLDER_CREATED is fired when locally created
 * folder is replicated to the server.
 */

#define PEVENT_TYPE_LOCAL            (0<<0)
#define PEVENT_TYPE_REMOTE           (1<<0)
#define PEVENT_TYPE_FILE             (0<<1)
#define PEVENT_TYPE_FOLDER           (1<<1)
#define PEVENT_TYPE_CREATE           (0<<2)
#define PEVENT_TYPE_DELETE           (1<<2)
#define PEVENT_TYPE_RENAME           (2<<2)
#define PEVENT_TYPE_START            (0<<5)
#define PEVENT_TYPE_FINISH           (1<<5)
#define PEVENT_TYPE_SUCCESS          (0<<6)
#define PEVENT_TYPE_FAIL             (1<<6)

#define PEVENT_FIRST_USER_EVENT      (1<<30)
#define PEVENT_FIRST_SHARE_EVENT     (PEVENT_FIRST_USER_EVENT+200)

#define PEVENT_LOCAL_FOLDER_CREATED   (PEVENT_TYPE_LOCAL+PEVENT_TYPE_FOLDER+PEVENT_TYPE_CREATE)
#define PEVENT_REMOTE_FOLDER_CREATED  (PEVENT_TYPE_REMOTE+PEVENT_TYPE_FOLDER+PEVENT_TYPE_CREATE)
#define PEVENT_FILE_DOWNLOAD_STARTED  (PEVENT_TYPE_LOCAL+PEVENT_TYPE_FILE+PEVENT_TYPE_CREATE+PEVENT_TYPE_START)
#define PEVENT_FILE_DOWNLOAD_FINISHED (PEVENT_TYPE_LOCAL+PEVENT_TYPE_FILE+PEVENT_TYPE_CREATE+PEVENT_TYPE_FINISH+PEVENT_TYPE_SUCCESS)
#define PEVENT_FILE_DOWNLOAD_FAILED   (PEVENT_TYPE_LOCAL+PEVENT_TYPE_FILE+PEVENT_TYPE_CREATE+PEVENT_TYPE_FINISH+PEVENT_TYPE_FAIL)
#define PEVENT_FILE_UPLOAD_STARTED    (PEVENT_TYPE_REMOTE+PEVENT_TYPE_FILE+PEVENT_TYPE_CREATE+PEVENT_TYPE_START)
#define PEVENT_FILE_UPLOAD_FINISHED   (PEVENT_TYPE_REMOTE+PEVENT_TYPE_FILE+PEVENT_TYPE_CREATE+PEVENT_TYPE_FINISH+PEVENT_TYPE_SUCCESS)
#define PEVENT_FILE_UPLOAD_FAILED     (PEVENT_TYPE_REMOTE+PEVENT_TYPE_FILE+PEVENT_TYPE_CREATE+PEVENT_TYPE_FINISH+PEVENT_TYPE_FAIL)
#define PEVENT_LOCAL_FOLDER_DELETED   (PEVENT_TYPE_LOCAL+PEVENT_TYPE_FOLDER+PEVENT_TYPE_DELETE)
#define PEVENT_REMOTE_FOLDER_DELETED  (PEVENT_TYPE_REMOTE+PEVENT_TYPE_FOLDER+PEVENT_TYPE_DELETE)
#define PEVENT_LOCAL_FILE_DELETED     (PEVENT_TYPE_LOCAL+PEVENT_TYPE_FILE+PEVENT_TYPE_DELETE)
#define PEVENT_REMOTE_FILE_DELETED    (PEVENT_TYPE_REMOTE+PEVENT_TYPE_FILE+PEVENT_TYPE_DELETE)
#define PEVENT_LOCAL_FOLDER_RENAMED   (PEVENT_TYPE_LOCAL+PEVENT_TYPE_FOLDER+PEVENT_TYPE_RENAME)

#define PEVENT_USERINFO_CHANGED PEVENT_FIRST_USER_EVENT
#define PEVENT_USEDQUOTA_CHANGED (PEVENT_FIRST_USER_EVENT+1)

#define PEVENT_SHARE_REQUESTIN   PEVENT_FIRST_SHARE_EVENT
#define PEVENT_SHARE_REQUESTOUT  (PEVENT_FIRST_SHARE_EVENT+1)
#define PEVENT_SHARE_ACCEPTIN    (PEVENT_FIRST_SHARE_EVENT+2)
#define PEVENT_SHARE_ACCEPTOUT   (PEVENT_FIRST_SHARE_EVENT+3)
#define PEVENT_SHARE_DECLINEIN   (PEVENT_FIRST_SHARE_EVENT+4)
#define PEVENT_SHARE_DECLINEOUT  (PEVENT_FIRST_SHARE_EVENT+5)
#define PEVENT_SHARE_CANCELIN    (PEVENT_FIRST_SHARE_EVENT+6)
#define PEVENT_SHARE_CANCELOUT   (PEVENT_FIRST_SHARE_EVENT+7)
#define PEVENT_SHARE_REMOVEIN    (PEVENT_FIRST_SHARE_EVENT+8)
#define PEVENT_SHARE_REMOVEOUT   (PEVENT_FIRST_SHARE_EVENT+9)
#define PEVENT_SHARE_MODIFYIN    (PEVENT_FIRST_SHARE_EVENT+10)
#define PEVENT_SHARE_MODIFYOUT   (PEVENT_FIRST_SHARE_EVENT+11)

#define PSYNC_DOWNLOAD_ONLY  1
#define PSYNC_UPLOAD_ONLY    2
#define PSYNC_FULL           3
#define PSYNC_SYNCTYPE_MIN   1
#define PSYNC_SYNCTYPE_MAX   3

#define PERROR_LOCAL_FOLDER_NOT_FOUND   1
#define PERROR_REMOTE_FOLDER_NOT_FOUND  2
#define PERROR_DATABASE_OPEN            3
#define PERROR_NO_HOMEDIR               4
#define PERROR_SSL_INIT_FAILED          5
#define PERROR_DATABASE_ERROR           6
#define PERROR_LOCAL_FOLDER_ACC_DENIED  7
#define PERROR_REMOTE_FOLDER_ACC_DENIED 8
#define PERROR_FOLDER_ALREADY_SYNCING   9
#define PERROR_INVALID_SYNCTYPE        10
#define PERROR_OFFLINE                 11
#define PERROR_INVALID_SYNCID          12
#define PERROR_PARENT_OR_SUBFOLDER_ALREADY_SYNCING 13
#define PERROR_LOCAL_IS_ON_PDRIVE      14

#define PLIST_FILES   1
#define PLIST_FOLDERS 2
#define PLIST_ALL     3

#define PSYNC_PERM_READ   1
#define PSYNC_PERM_CREATE 2
#define PSYNC_PERM_MODIFY 4
#define PSYNC_PERM_DELETE 8

#define PSYNC_PERM_ALL (PSYNC_PERM_READ|PSYNC_PERM_CREATE|PSYNC_PERM_MODIFY|PSYNC_PERM_DELETE)
#define PSYNC_PERM_WRITE (PSYNC_PERM_CREATE|PSYNC_PERM_MODIFY|PSYNC_PERM_DELETE)

#define PSYNC_CRYPTO_SETUP_SUCCESS       0
#define PSYNC_CRYPTO_SETUP_NOT_SUPPORTED -1
#define PSYNC_CRYPTO_SETUP_KEYGEN_FAILED 1
#define PSYNC_CRYPTO_SETUP_CANT_CONNECT  2
#define PSYNC_CRYPTO_SETUP_NOT_LOGGED_IN 3
#define PSYNC_CRYPTO_SETUP_ALREADY_SETUP 4
#define PSYNC_CRYPTO_SETUP_UNKNOWN_ERROR 5

#define PSYNC_CRYPTO_START_SUCCESS         0
#define PSYNC_CRYPTO_START_NOT_SUPPORTED   -1
#define PSYNC_CRYPTO_START_ALREADY_STARTED 1 
#define PSYNC_CRYPTO_START_CANT_CONNECT    2
#define PSYNC_CRYPTO_START_NOT_LOGGED_IN   3
#define PSYNC_CRYPTO_START_NOT_SETUP       4
#define PSYNC_CRYPTO_START_UNKNOWN_KEY_FORMAT 5
#define PSYNC_CRYPTO_START_BAD_PASSWORD    6
#define PSYNC_CRYPTO_START_KEYS_DONT_MATCH 7
#define PSYNC_CRYPTO_START_UNKNOWN_ERROR   8

#define PSYNC_CRYPTO_STOP_SUCCESS          0
#define PSYNC_CRYPTO_STOP_NOT_SUPPORTED    -1
#define PSYNC_CRYPTO_STOP_NOT_STARTED      1

#define PSYNC_CRYPTO_HINT_SUCCESS          0
#define PSYNC_CRYPTO_HINT_NOT_SUPPORTED    -1
#define PSYNC_CRYPTO_HINT_NOT_PROVIDED     1
#define PSYNC_CRYPTO_HINT_CANT_CONNECT     2
#define PSYNC_CRYPTO_HINT_NOT_LOGGED_IN    3
#define PSYNC_CRYPTO_HINT_UNKNOWN_ERROR    4

#define PSYNC_CRYPTO_RESET_SUCCESS         0
#define PSYNC_CRYPTO_RESET_CRYPTO_IS_STARTED 1
#define PSYNC_CRYPTO_RESET_CANT_CONNECT    2
#define PSYNC_CRYPTO_RESET_NOT_LOGGED_IN   3
#define PSYNC_CRYPTO_RESET_NOT_SETUP       4
#define PSYNC_CRYPTO_RESET_UNKNOWN_ERROR   5

#define PSYNC_CRYPTO_SUCCESS               0
#define PSYNC_CRYPTO_NOT_STARTED           -1
#define PSYNC_CRYPTO_RSA_ERROR             -2
#define PSYNC_CRYPTO_FOLDER_NOT_FOUND      -3
#define PSYNC_CRYPTO_FILE_NOT_FOUND        -4
#define PSYNC_CRYPTO_INVALID_KEY           -5
#define PSYNC_CRYPTO_CANT_CONNECT          -6
#define PSYNC_CRYPTO_FOLDER_NOT_ENCRYPTED  -7
#define PSYNC_CRYPTO_INTERNAL_ERROR        -8

#define PSYNC_CRYPTO_INVALID_FOLDERID      ((psync_folderid_t)-1)

typedef struct {
  const char *localname;
  const char *localpath;
  const char *remotename;
  const char *remotepath;
  psync_folderid_t folderid;
  psync_syncid_t syncid;
  psync_synctype_t synctype;
} psync_folder_t;

typedef struct {
  size_t foldercnt;
  psync_folder_t folders[];
} psync_folder_list_t;

typedef struct {
  psync_fileid_t fileid;
  const char *name;
  const char *localpath;
  const char *remotepath;
  psync_syncid_t syncid;                                  
} psync_file_event_t;

typedef struct {
  psync_fileid_t folderid;
  const char *name;
  const char *localpath;
  const char *remotepath;
  psync_syncid_t syncid;                                  
} psync_folder_event_t;

typedef struct {
  psync_folderid_t folderid;
  const char *sharename;
  const char *email;
  const char *message;
  psync_userid_t userid;
  psync_shareid_t shareid;
  psync_sharerequestid_t sharerequestid;
  time_t created;
  unsigned char canread;
  unsigned char cancreate;
  unsigned char canmodify;
  unsigned char candelete;
} psync_share_event_t;

typedef union {
  psync_file_event_t *file;
  psync_folder_event_t *folder;
  psync_share_event_t *share;
  void *ptr;
} psync_eventdata_t;

typedef struct {
  psync_sharerequestid_t sharerequestid;
  psync_folderid_t folderid;
  time_t created;
  psync_userid_t userid;
  const char *email;
  const char *sharename;
  const char *message;
  unsigned char permissions;
  unsigned char canread;
  unsigned char cancreate;
  unsigned char canmodify;
  unsigned char candelete;
} psync_sharerequest_t;

typedef struct {
  size_t sharerequestcnt;
  psync_sharerequest_t sharerequests[];
} psync_sharerequest_list_t;

typedef struct {
  psync_shareid_t shareid;
  psync_folderid_t folderid;
  time_t created;
  psync_userid_t userid;
  const char *email;
  const char *sharename;
  unsigned char permissions;
  unsigned char canread;
  unsigned char cancreate;
  unsigned char canmodify;
  unsigned char candelete;
} psync_share_t;

typedef struct {
  size_t sharecnt;
  psync_share_t shares[];
} psync_share_list_t;

typedef struct {
  const char *url;
  const char *notes;
  const char *versionstr;
  const char *localpath;
  unsigned long version;
  uint64_t updatesize;
} psync_new_version_t;

#define PSYNC_INVALID_SYNCID (psync_syncid_t)-1

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*psync_malloc_t)(size_t);
typedef void *(*psync_realloc_t)(void *, size_t);
typedef void (*psync_free_t)(void *);

extern psync_malloc_t psync_malloc;
extern psync_realloc_t psync_realloc;
extern psync_free_t psync_free;

/* Status change callback is called every time value is changed. It may be called quite often
 * when there are active uploads/downloads. Callbacks are issued from a special callback thread
 * (e.g. the same thread all the time) and are guaranteed not to overlap.
 */

typedef void (*pstatus_change_callback_t)(pstatus_t *status);


/* Event callback is called every time a download/upload is started/finished,
 * quota is changed, folder is shared or similar. Look at the PEVENT_ constants
 * for a list of possible events.
 * 
 * The type of data parameter is specific to eventtype. That is for PEVENT_FILE_* or
 * PEVENT_*_FILE_* events the type is psync_folder_event_t, for PEVENT_*_FOLDER_*
 * events it is psync_file_event_t, for PEVENT_SHARE_* events is psync_share_event_t.
 * For PEVENT_USERINFO_CHANGED and PEVENT_USEDQUOTA_CHANGED data is NULL. Observe
 * the changes by calling psync_get_*_value() functions.
 * 
 * It is unsafe to use pointers to strings that are passed in the data structure, 
 * if you need to use them this way, strdup() will do the job. Event callbacks will 
 * not overlap.
 *
 * Do not expect localpath to exist after receiving PEVENT_FILE_DOWNLOAD_STARTED
 * as the file will be created with alternative name first and renamed when download
 * is finished.
 * 
 */

typedef void (*pevent_callback_t)(psync_eventtype_t event, psync_eventdata_t data);

/* psync_init inits the sync library. No network or local scan operations are initiated
 * by this call, call psync_start_sync to start those. However listing remote folders,
 * listing and editing syncs is supported.
 *
 * Returns 0 on success and -1 otherwise.
 *
 * psync_start_sync starts remote sync, both callbacks can be NULL, but most of the time setting
 * at least status_callback will make sense. Applications should expect immediate
 * status_callback with status of PSTATUS_LOGIN_REQUIRED after first run of psync_start_sync().
 *
 * psync_download_state is to be called after psync_init but before/instead of psync_start_sync.
 * This function downloads the directory structure into the local state in foreground (e.g. it can
 * take time to complete). It returns one of PSTATUS_-es, specifically PSTATUS_READY, PSTATUS_OFFLINE
 * or one of login-related statuses. After a successful call to this function remote folder listing can
 * be preformed.
 *
 * psync_destroy is to be called before application exit. This is not neccessary.
 * In any case psync_destroy will return relatively fast, regardless of blocked
 * network calls and other potentially slow to finish tasks.
 *
 * psync_set_alloc can set the allocator to be used by the library. To be called
 * BEFORE psync_init if ever. If allocator is provided, its free() function is to
 * be used to free any memory that is said to be freed when returned by the library.
 *
 * psync_set_database_path can set a full path to database file. If it does not exists
 * it will be created. The function should be only called before psync_init. If
 * database path is not set, appropriate location for the given OS will be chosen.
 * The library will make it's own copy of the path, so the memory can be free()d/reused
 * after the function returns. The path is not checked by the function itself, if it is
 * invalid (directory does not exist, it is not writable, etc) psync_init will return
 * -1 and the error code will be PERROR_DATABASE_OPEN. In this condition it is safe to
 * call psync_set_database_path and psync_init again. A special value of ":memory:" for
 * databasepath will create in-memory database that will not be preserved between runs.
 * An empty string will create the database in a temporary file, the net effect being
 * similar to the ":memory:" option with less pressure on the required memory. The
 * underlying database is in fact SQLite, so any other options that work for SQLite will
 * work here.
 *
 */

void psync_set_database_path(const char *databasepath);
void psync_set_alloc(psync_malloc_t malloc_call, psync_realloc_t realloc_call, psync_free_t free_call);

int psync_init();
void psync_start_sync(pstatus_change_callback_t status_callback, pevent_callback_t event_callback);
uint32_t psync_download_state();
void psync_destroy();

/* returns current status.
 */

void psync_get_status(pstatus_t *status);

/* psync_set_user_pass and psync_set_auth functions can be used for initial login
 * (PSTATUS_LOGIN_REQUIRED) and when PSTATUS_BAD_LOGIN_DATA error is returned, however
 * if the username do not match previously logged in user, PSTATUS_USER_MISMATCH event
 * will be generated. Preferably on PSTATUS_BAD_LOGIN_DATA the user should be only prompted
 * for new password and psync_set_pass should be called. To change the current user, psync_unlink
 * is to be called first and then the new user may log in.
 *
 * The pointer returned by psync_get_username() is to be free()d.
 */

char *psync_get_username();
void psync_set_user_pass(const char *username, const char *password, int save);
void psync_set_pass(const char *password, int save);
void psync_set_auth(const char *auth, int save);
void psync_logout();
void psync_unlink();

/* psync_add_sync_by_path and psync_add_sync_by_folderid are to be used to add a folder to be synced,
 * on success syncid is returned, on error -1. The value of synctype should always be one of PSYNC_DOWNLOAD_ONLY,
 * PSYNC_UPLOAD_ONLY or PSYNC_FULL.
 *
 * psync_add_sync_by_path_delayed generally works in a way similar to psync_add_sync_by_path, but with few
 * differences:
 *  1) it can be called just after psync_init() and before psync_start_sync(), even before logging in and downloading account state
 *  2) actual creation of the sync will be delayed until login and state download
 *  3) if remotepath does not exist, it will be created if possible (it is generally possible to create any path
 *     unless some prefix of the path is a mounted share with no create privileges)
 *  4) in rare cases when remote path does not exists and could not be created, whole psync_add_sync_by_path_delayed
 *     request will be silently discarded
 *  5) psync_add_sync_by_path_delayed does not return syncid, and can only fail if there is some problem with localpath
 *
 * psync_change_synctype changes the sync type, on success returns 0 and -1 on error.
 *
 * psync_delete_sync deletes the sync relationship between folders, on success returns 0
 * and -1 on error (it is only likely to fail if syncid is invalid). No files or folders
 * are deleted either in the cloud or locally.
 *
 * psync_get_sync_list returns all folders that are set for sync. On error returns NULL.
 * On success the returned pointer is to be free()d.
 *
 */

psync_syncid_t psync_add_sync_by_path(const char *localpath, const char *remotepath, psync_synctype_t synctype);
psync_syncid_t psync_add_sync_by_folderid(const char *localpath, psync_folderid_t folderid, psync_synctype_t synctype);
int psync_add_sync_by_path_delayed(const char *localpath, const char *remotepath, psync_synctype_t synctype);
int psync_change_synctype(psync_syncid_t syncid, psync_synctype_t synctype);
int psync_delete_sync(psync_syncid_t syncid);
psync_folder_list_t *psync_get_sync_list();

psuggested_folders_t *psync_get_sync_suggestions();

/* Use the following functions to list local or remote folders.
 * For local folders fileid and folderid will be set to a value that
 * should in general uniquely identify the entry (e.g. inode number).
 * Remote paths use slashes (/) and start with one.
 * In case of success the returned folder list is to be freed with a
 * single call to free(). In case of error NULL is returned. Parameter
 * listtype should be one of PLIST_FILES, PLIST_FOLDERS or PLIST_ALL.
 *
 * Folders do not contain "." or ".." entries.
 *
 * All files/folders are listed regardless if they are to be ignored
 * based on 'ignorepatterns' setting. If needed, pass the names to
 * psync_is_name_to_ignore that returns 1 for files that are to be
 * ignored and 0 for others.
 *
 * Remote root folder has 0 folderid.
 */

pfolder_list_t *psync_list_local_folder_by_path(const char *localpath, psync_listtype_t listtype);
pfolder_list_t *psync_list_remote_folder_by_path(const char *remotepath, psync_listtype_t listtype);
pfolder_list_t *psync_list_remote_folder_by_folderid(psync_folderid_t folderid, psync_listtype_t listtype);
pentry_t *psync_stat_path(const char *remotepath);
int psync_is_name_to_ignore(const char *name);

/* Returns the code of the last error that occured when calling psync_* functions
 * in the given thread. The error is one of PERROR_* constants.
 */

uint32_t psync_get_last_error();

/* Pause stops the sync, but both local and remote directories are still
 * monitored for updates and status updates are still received with updated
 * filestoupload/filestodownload and others.
 *
 * Stop stops all the actions of the library. No network traffic and no local
 * scans are to be expected after this call. No status updates callback except
 * the one setting PSTATUS_STOPPED status.
 *
 * Resume will restart all operations in both paused and stopped state.
 */

int psync_pause();
int psync_stop();
int psync_resume();

/* Forces rescan of local files and folders. You generally don't need to call this function.
 * It can be useful only as an option for the user of the program to force local re-scan.
 */

void psync_run_localscan();

/* Registers a new user account. email is user e-mail address which will also be
 * the username after successful registration. Password is user's chosen password
 * implementations are advised to have the user verify the password by typing it
 * twice. The termsaccepted field should only be set to true if the user actually
 * indicated acceptance of pCloud terms and conditions.
 *
 * Returns zero on success, -1 if network error occurs or a positive error code from
 * this list:
 * https://docs.pcloud.com/methods/auth/register.html
 * In case of error.
 *
 * If err is not NULL in all cases of non-zero return it will be set to point to a
 * psync_malloc-allocated buffer with English language error text, suitable to display
 * to the user. This buffer must be freed by the application.
 * 
 */

int psync_register(const char *email, const char *password, int termsaccepted, char **err);

/* Sends email verification mail to the user, return value and err are the same as with registering.
 */

int psync_verify_email(char **err);

/* Sends email with link to reset password to the user with specified email, return value and err are 
 * the same as with registering.
 */

int psync_lost_password(const char *email, char **err);

/* Changes the password of the user, return value and err are the same as with registering.
 */

int psync_change_password(const char *currentpass, const char *newpass, char **err);

int psync_create_remote_folder_by_path(const char *path, char **err);

int psync_create_remote_folder(psync_folderid_t parentfolderid, const char *name, char **err);

/* Returns auth string of the current user. Do not free the returned string (which should be obvious from
 * the const anyway).
 */

const char *psync_get_auth_string();

/*
 * List of settings:
 * usessl (bool) - use SSL connections to remote servers
 * maxdownloadspeed (int) - maximum download speed in bytes per second, 0 for auto-shaper, -1 for no limit
 * maxuploadspeed (int) - maximum upload speed in bytes per second, 0 for auto-shaper, -1 for no limit
 * minlocalfreespace (uint) - minimum free space on local drives to run downloads
 * ignorepatterns (string) - patterns of files and folders to be ignored when syncing, separated by ";" supported widcards are
 *                          * - matches any number of characters (even zero)
 *                          ? - matches exactly one character
 * p2psync (bool) - use or not peer to peer downloads
 * 
 * fscachesize (uint) - size of filesystem cache, in bytes, sane minimum of few tens of Mb or even hundreds is advised
 * fsroot (string) - where to mount the filesystem
 * autostartfs (bool) - if set starts the fs on app startup
 * sleepstopcrypto (bool) - if set, stops crypto when computer wakes up from sleep
 * 
 *
 * The following functions operate on settings. The value of psync_get_string_setting does not have to be freed, however if you are
 * going to store it rather than use it right away, you should strdup() it.
 *
 * psync_set_*_setting functions return 0 on success and -1 on failure. Setting a setting may fail if you mismatch the type or give
 * invalid setting name.
 *
 * psync_get_string_setting returns empty string on failure (type mismatch or non-existing setting), all other psync_get_*_setting
 * return zero on failure.
 *
 * All settings are reset to default values on unlink.
 *
 * int and uint are interchangeable and are considered same type.
 *
 */

int psync_get_bool_setting(const char *settingname);
int psync_set_bool_setting(const char *settingname, int value);
int64_t psync_get_int_setting(const char *settingname);
int psync_set_int_setting(const char *settingname, int64_t value);
uint64_t psync_get_uint_setting(const char *settingname);
int psync_set_uint_setting(const char *settingname, uint64_t value);
const char *psync_get_string_setting(const char *settingname);
int psync_set_string_setting(const char *settingname, const char *value);

/*
 * Values are like settings, except that you can store and retrieve any key-value pair you want. There are some library-polpulated
 * values that you are not supposed to change. There are no type mismatch for values, instead they are converted to requested
 * representation.
 *
 * The pointer returned by psync_get_string_value is to be freed by the application. This function returns NULL when value does not
 * exist (as opposed to psync_get_string_setting).
 *
 * The application can store values even when there is no user logged in. However all values are cleared on unlink.
 *
 * Library-populated values are:
 * dbversion (uint) - version of the database
 * runstatus (uint) - one of (1, 2, 4) for current run status of (run, pause, stop)
 * saveauth  (bool) - indicates whether user've chosen to save the password or not
 * userid    (uint) - userid of the logged in user
 * username  (string)- username/email of the logged in user
 * emailverified (bool)- indicates whether the user's email is verified
 * premium   (bool) - true if user is paid account
 * premiumexpires (uint) - if premium is true, the expire date of the premium account in unix timestamp
 * language  (string) - two letter, lowercase ISO 639-1 code of the user's language preference
 * quota (uint) - user's quota in bytes
 * usedquota (uint) - used space of the quota in bytes
 * diffid (uint) - diffid of the user status, see https://docs.pcloud.com/methods/general/diff.html, can be used to detect account changes
 * auth (string) - user's auth token (if the user is logged in and saveauth is true), see https://docs.pcloud.com/methods/intro/authentication.html
 *
 */

int psync_has_value(const char *valuename);
int psync_get_bool_value(const char *valuename);
void psync_set_bool_value(const char *valuename, int value);
int64_t psync_get_int_value(const char *valuename);
void psync_set_int_value(const char *valuename, int64_t value);
uint64_t psync_get_uint_value(const char *valuename);
void psync_set_uint_value(const char *valuename, uint64_t value);
char *psync_get_string_value(const char *valuename);
void psync_set_string_value(const char *valuename, const char *value);

/* If your application has a way to detect network change (e.g. wireless access point change), you should subscribe for
 * such notifications and call psync_network_exception() in those cases. There is no harm in calling it too often.
 * 
 */

void psync_network_exception();

/* The following functions return lists of pending sharerequests in psync_list_sharerequests and
 * list of shared folders in case of psync_list_shares. Memory is to be freed with a single free().
 * 
 * These functions do not return errors. However, if no user is logged in, they will return empty lists.
 * 
 * Pass 1 as parameter to list incoming sharerequests/shares and 0 for outgoing.
 * 
 * Listing shares/sharerequests do not require active network connection.
 * 
 */

psync_sharerequest_list_t *psync_list_sharerequests(int incoming);
psync_share_list_t *psync_list_shares(int incoming);

/* psync_share_folder shares a folder with the user "mail". The "permissions" parameter is bitwise or of
 * PSYNC_PERM_READ, PSYNC_PERM_CREATE, PSYNC_PERM_MODIFY and PSYNC_PERM_DELETE (PSYNC_PERM_READ is actually
 * ignored and always set).
 * 
 * On success returns 0, otherwise returns API error number (or -1 on network error) and sets err to a string
 * error message if it is not NULL. This string should be freed if the return value is not 0 and err is not NULL.
 * 
 * It is NOT guaranteed that upon successful return psync_list_sharerequests(0) will return the newly created 
 * share request. Windows showing list of sharerequests/shares are supposed to requery shares/request upon receiving of
 * PEVENT_SHARE_* event. That is true for all share management functions.
 * 
 */

int psync_share_folder(psync_folderid_t folderid, const char *name, const char *mail, const char *message, uint32_t permissions, char **err);


/* Cancels a share request (this is to be called for outgoing requests).
 * 
 * Return value same as psync_share_folder.
 */

int psync_cancel_share_request(psync_sharerequestid_t requestid, char **err);

/* Declines a share request (this is to be called for incoming requests).
 * 
 * Return value same as psync_share_folder.
 */

int psync_decline_share_request(psync_sharerequestid_t requestid, char **err);

/* Accepts a share request to a folder "tofolderid" under a name "name". If "name" is NULL then the original share name is used.
 * 
 * Return value same as psync_share_folder.
 */

int psync_accept_share_request(psync_sharerequestid_t requestid, psync_folderid_t tofolderid, const char *name, char **err);

/* Removes established share. Can be called by both receiving and sharing user.
 * 
 * Return value same as psync_share_folder.
 */

int psync_remove_share(psync_shareid_t shareid, char **err);

/* Removes established share. Can be called by both receiving and sharing user.
 * 
 * Return value same as psync_share_folder.
 */

int psync_modify_share(psync_shareid_t shareid, uint32_t permissions, char **err);

/* The following function check for new version of the application. Return NULL if there is no new
 * version or psync_new_version_t structure if a new version is available. Returned value is to be
 * freed with a single free(). The os parameter is one of the following:
 * WIN
 * WIN_XP
 * MAC
 * LINUX32
 * LINUX64
 * 
 * String versions are in format "a.b.c", equivalen numeric version is a*10000+b*100+c
 * 
 * The _download version also downloads (and is potentially slow) the update and stores it in 
 * (returned value)->localpath.
 *
 * Function psync_run_new_version actually runs the update. On success it exists and does not return.
 * 
 * Applications are expected to run psync_check_new_version and upon non-NULL return to seek user
 * confirmation for updating and if the user confirms run psync_run_new_version
 * 
 */

psync_new_version_t *psync_check_new_version_str(const char *os, const char *currentversion);
psync_new_version_t *psync_check_new_version(const char *os, unsigned long currentversion);
psync_new_version_t *psync_check_new_version_download_str(const char *os, const char *currentversion);
psync_new_version_t *psync_check_new_version_download(const char *os, unsigned long currentversion);
void psync_run_new_version(psync_new_version_t *ver);

/* Filesystem functions.
 * 
 * psync_fs_start() - starts the filesystem
 * psync_fs_isstarted() - returns 1 if the filesystem is started and 0 otherwise
 * psync_fs_stop() - stops the filesystem
 * psync_fs_getmountpoint() - returns current mountpoint of the filesystem, or NULL if the filesystem is not mounted,
 *                            you are supposed to free the returned pointer
 * psync_fs_get_path_by_folderid() - returns full path (including mountpoint) of a given folderid on the filesystem or
 *                            NULL if it is not mounted or folder could not be found. You are supposed to free the returned
 *                            pointer.
 * 
 */

int psync_fs_start();
int psync_fs_isstarted();
void psync_fs_stop();
char *psync_fs_getmountpoint();
char *psync_fs_get_path_by_folderid(psync_folderid_t folderid);


/* psync_password_quality estimates password quality, returns one of:
 *   0 - weak
 *   1 - moderate
 *   2 - strong
 */

int psync_password_quality(const char *password);

/* psync_password_quality10000 works the same way as psync_password_quality but for each password strength also return
 * a range, returns integer in one of the following (inclusive) intervals
 *   0     to  9999 - weak password
 *   10000 to 19999 - moderate password
 *   20000 to 29999 - strong password
 * 
 * integer division of the result of psync_password_quality10000 by 10000 will give the same result as psync_password_quality()
 * 
 */

int psync_password_quality10000(const char *password);

/*
 * Crypto functions.
 * 
 * psync_crypto_setup() - setups crypto with a given password, on error returns one of PSYNC_CRYPTO_SETUP_* errors
 * psync_crypto_get_hint() - if successful sets *hint to point to a string with the user's password hint. In this case
 *                        *hint is to be free-d. On error one of PSYNC_CRYPTO_HINT_* codes is returned and *hint is not
 *                        set.
 * psync_crypto_start() - starts crypto with a given password, on error returns one of PSYNC_CRYPTO_START_* errors
 * psync_crypto_stop() - stops crypto, on error returns one of PSYNC_CRYPTO_STOP_* errors
 * psync_crypto_isstarted() - returns 1 if crypto is started and 0 otherwise
 * psync_crypto_mkdir() - creates encrypted folder with name in folderid. If the parent 
 *                        folder is not encrypted itself the folder name will be stored in plaintext
 *                        and only the contents will be encrypted. Returns 0 for success and sets *newfolderid (if newfolderid is
 *                        non-NULL) to the id of the new folder, or non-zero on error. Negative error values are local and positive
 *                        error  values are API error codes. If err is not null it is set to point to an static error string
 *                        message that you do NOT have to free.
 * psync_crypto_issetup() - returns 1 if crypto is set up or 0 otherwise
 * psync_crypto_hassubscription() - returns 1 if the user have active payment subscription for crypto or 0 otherwise
 * psync_crypto_isexpired() - returns 1 if the users crypto service is expired or 0 otherwise. Note that it also returns
 *                        0 when the user never set up crypto and is therefore eligible for a trial account.
 * psync_crypto_expires() - returns unix timestamp with the date of current crypto service expiration. The returned value
 *                        may be in the past, meaning expired service. If crypto has never been setup for this account
 *                        this functions returns 0.
 * psync_crypto_reset() - reset user's crypto, which means that all encrypted files and folders get deleted. This function
 *                        does not directly reset user's account, a confirmation email is first sent to the user.
 * psync_crypto_folderid() - returns the id of the first encrypted folder it finds. If no encrypted folder is found the function returns
 *                        PSYNC_CRYPTO_INVALID_FOLDERID.
 * psync_crypto_folderids() - returns array of the ids of all encrypted folders (but not their subfolders). Last element of the array is
 *                        always PSYNC_CRYPTO_INVALID_FOLDERID. You need to free the memory returned by this function.
 * 
 * 
 */

int psync_crypto_setup(const char *password, const char *hint);
int psync_crypto_get_hint(char **hint);
int psync_crypto_start(const char *password);
int psync_crypto_stop();
int psync_crypto_isstarted();
int psync_crypto_mkdir(psync_folderid_t folderid, const char *name, const char **err, psync_folderid_t *newfolderid);
int psync_crypto_issetup();
int psync_crypto_hassubscription();
int psync_crypto_isexpired();
time_t psync_crypto_expires();
int psync_crypto_reset();
psync_folderid_t psync_crypto_folderid();
psync_folderid_t *psync_crypto_folderids();

#ifdef __cplusplus
}
#endif

#endif
