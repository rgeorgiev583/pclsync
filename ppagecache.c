/* Copyright (c) 2014 Anton Titov.
 * Copyright (c) 2014 pCloud Ltd.
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

#include "ppagecache.h"
#include "psettings.h"
#include "plibs.h"
#include "ptimer.h"
#include "pnetlibs.h"
#include "pstatus.h"
#include "pcache.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>

#define CACHE_PAGES (PSYNC_FS_MEMORY_CACHE/PSYNC_FS_PAGE_SIZE)
#define CACHE_HASH (CACHE_PAGES/2)

#define PAGE_WAITER_HASH 1024
#define PAGE_WAITER_MUTEXES 16

#define DB_CACHE_UPDATE_HASH (32*1024)

#define PAGE_TYPE_FREE 0
#define PAGE_TYPE_READ 1

#define PAGE_TASK_TYPE_CREAT  0
#define PAGE_TASK_TYPE_MODIFY 1

#define pagehash_by_hash_and_pageid(hash, pageid) (((hash)+(pageid))%CACHE_HASH)
#define waiterhash_by_hash_and_pageid(hash, pageid) (((hash)+(pageid))%PAGE_WAITER_HASH)
#define waiter_mutex_by_hash(hash) (hash%PAGE_WAITER_MUTEXES)
#define lock_wait(hash) pthread_mutex_lock(&wait_page_mutexes[waiter_mutex_by_hash(hash)])
#define unlock_wait(hash) pthread_mutex_unlock(&wait_page_mutexes[waiter_mutex_by_hash(hash)])

typedef struct {
  psync_list list;
  psync_list flushlist;
  char *page;
  uint64_t hash;
  uint64_t pageid;
  time_t lastuse;
  uint32_t size;
  uint32_t usecnt;
  uint32_t flushpageid;
  uint8_t type;
} psync_cache_page_t;

typedef struct {
  uint64_t pagecacheid;
  time_t lastuse;
  uint32_t usecnt;
} psync_cachepage_to_update;

typedef struct {
  /* list is an element of hash table for pages */
  psync_list list;
  /* list is root of a listpage elements of psync_page_waiter_t, if empty, nobody waits for this page */
  psync_list waiters;
  uint64_t hash;
  uint64_t pageid;
  psync_fileid_t fileid;
} psync_page_wait_t;

typedef struct {
  /* listpage is node element of psync_page_wait_t waiters list */
  psync_list listpage;
  /* listwaiter is node element of pages that are needed for current request */
  psync_list listwaiter;
  pthread_cond_t cond;
  psync_page_wait_t *waiting_for;
  char *buff;
  uint32_t pageidx;
  uint32_t rsize;
  uint32_t size;
  uint32_t off;
  int error;
  uint8_t ready;
} psync_page_waiter_t;

typedef struct {
  psync_list list;
  uint64_t offset;
  uint64_t length;
} psync_request_range_t;

typedef struct {
  psync_list ranges;
  psync_openfile_t *of;
  psync_fileid_t fileid;
  uint64_t hash;
} psync_request_t;

typedef struct {
  uint64_t hash;
  psync_tree tree;
  binresult *urls;
  uint32_t refcnt;
  uint32_t status;
} psync_urls_t;

static psync_list cache_hash[CACHE_HASH];
static uint32_t cache_pages_in_hash=0;
static uint32_t cache_pages_free;
static psync_list free_pages;
static psync_list wait_page_hash[PAGE_WAITER_HASH];
static char *pages_base;

static psync_cachepage_to_update cachepages_to_update[DB_CACHE_UPDATE_HASH];
static uint32_t cachepages_to_update_cnt=0;
static uint32_t free_db_pages;

static pthread_mutex_t clean_cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clean_cache_cond=PTHREAD_COND_INITIALIZER;
static pthread_mutex_t cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t flush_cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t url_cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t url_cache_cond=PTHREAD_COND_INITIALIZER;
static pthread_mutex_t wait_page_mutexes[PAGE_WAITER_MUTEXES];

static uint32_t clean_cache_stoppers=0;
static uint32_t clean_cache_waiters=0;

static int flushedbetweentimers=0;
static int flushchacherun=0;
static int upload_to_cache_thread_run=0;

static uint64_t db_cache_in_pages;
static uint64_t db_cache_max_page;

static psync_file_t readcache=INVALID_HANDLE_VALUE;

static psync_tree *url_cache_tree=PSYNC_TREE_EMPTY;

static int flush_pages(int nosleep);

static void flush_pages_noret(){
  flush_pages(0);
}

static psync_cache_page_t *psync_pagecache_get_free_page(){
  psync_cache_page_t *page;
  pthread_mutex_lock(&cache_mutex);
  if (cache_pages_free<=CACHE_PAGES*10/100 && !flushchacherun){
    psync_run_thread("flush pages get free page", flush_pages_noret);
    flushchacherun=1;
  }
  if (likely(!psync_list_isempty(&free_pages)))
    page=psync_list_remove_head_element(&free_pages, psync_cache_page_t, list);
  else{
    debug(D_NOTICE, "no free pages, flushing cache");
    pthread_mutex_unlock(&cache_mutex);
    flush_pages(1);
    pthread_mutex_lock(&cache_mutex);
    while (unlikely(psync_list_isempty(&free_pages))){
      pthread_mutex_unlock(&cache_mutex);
      debug(D_NOTICE, "no free pages after flush, sleeping");
      psync_milisleep(200);
      flush_pages(1);
      pthread_mutex_lock(&cache_mutex);
    }
    page=psync_list_remove_head_element(&free_pages, psync_cache_page_t, list);
  }
  cache_pages_free--;
  pthread_mutex_unlock(&cache_mutex);
  return page;
}

static int psync_api_send_read_request(psync_socket *api, psync_fileid_t fileid, uint64_t hash, uint64_t offset, uint64_t length){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("fileid", fileid), P_NUM("hash", hash), P_NUM("offset", offset), P_NUM("count", length)};
  return send_command_no_res(api, "readfile", params)==PTR_OK?0:-1;
}

static int psync_api_send_read_request_thread(psync_socket *api, psync_fileid_t fileid, uint64_t hash, uint64_t offset, uint64_t length){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("fileid", fileid), P_NUM("hash", hash), P_NUM("offset", offset), P_NUM("count", length)};
  return send_command_no_res_thread(api, "readfile", params)==PTR_OK?0:-1;
}


static void psync_pagecache_send_page_wait_page(psync_page_wait_t *pw, psync_cache_page_t *page){
  psync_page_waiter_t *pwt;
  psync_list_del(&pw->list);
  psync_list_for_each_element(pwt, &pw->waiters, psync_page_waiter_t, listpage){
    page->usecnt++;
    if (pwt->off+pwt->size>page->size){
      if (pwt->off>=page->size)
        pwt->rsize=0;
      else
        pwt->rsize=page->size-pwt->off;
    }
    else
      pwt->rsize=pwt->size;
    memcpy(pwt->buff, page->page+pwt->off, pwt->rsize);
    pwt->error=0;
    pwt->ready=1;
    pthread_cond_broadcast(&pwt->cond);
  }
  psync_free(pw);
}

static void psync_pagecache_return_free_page_locked(psync_cache_page_t *page){
  psync_list_add_head(&free_pages, &page->list);
  cache_pages_free++;
}

static void psync_pagecache_return_free_page(psync_cache_page_t *page){
  pthread_mutex_lock(&cache_mutex);
  psync_pagecache_return_free_page_locked(page);
  pthread_mutex_unlock(&cache_mutex);
}

static int psync_pagecache_read_range_from_api(psync_request_t *request, psync_request_range_t *range, psync_socket *api){
  uint64_t first_page_id, dlen;
  psync_page_wait_t *pw;
  psync_cache_page_t *page;
  binresult *res;
  psync_uint_t len, i, h;
  int rb;
  first_page_id=range->offset/PSYNC_FS_PAGE_SIZE;
  len=range->length/PSYNC_FS_PAGE_SIZE;
  res=get_result_thread(api);
  if (unlikely_log(!res))
    return -2;
  i=psync_find_result(res, "result", PARAM_NUM)->num;
  if (unlikely(i)){
    psync_free(res);
    debug(D_WARNING, "readfile returned error %lu", (long unsigned)i);
    return -2;
  }
  dlen=psync_find_result(res, "data", PARAM_DATA)->num;
  psync_free(res);
  for (i=0; i<len; i++){
    page=psync_pagecache_get_free_page();
    rb=psync_socket_readall_download_thread(api, page->page, dlen<PSYNC_FS_PAGE_SIZE?dlen:PSYNC_FS_PAGE_SIZE);
    if (unlikely_log(rb<=0)){
      psync_pagecache_return_free_page(page);
      psync_timer_notify_exception();
      return i==0?-2:-1;
    }
    dlen-=rb;
    page->hash=request->of->hash;
    page->pageid=first_page_id+i;
    page->lastuse=psync_timer_time();
    page->size=rb;
    page->usecnt=0;
    page->type=PAGE_TYPE_READ;
    h=waiterhash_by_hash_and_pageid(page->hash, page->pageid);
    lock_wait(page->hash);
    psync_list_for_each_element(pw, &wait_page_hash[h], psync_page_wait_t, list)
      if (pw->hash==page->hash && pw->pageid==page->pageid){
        psync_pagecache_send_page_wait_page(pw, page);
        break;
      }
    unlock_wait(page->hash);
    pthread_mutex_lock(&cache_mutex);
    psync_list_add_tail(&cache_hash[pagehash_by_hash_and_pageid(page->hash, page->pageid)], &page->list);
    cache_pages_in_hash++;
    pthread_mutex_unlock(&cache_mutex);
  }
  return 0;
}

typedef struct {
  psync_list list;
  pthread_cond_t cond;
  psync_socket *api;
} shared_api_waiter_t;

static pthread_mutex_t sharedapi_mutex=PTHREAD_MUTEX_INITIALIZER;
static psync_socket *sharedapi=NULL;
static psync_list sharedapiwaiters=PSYNC_LIST_STATIC_INIT(sharedapiwaiters);

static void mark_api_shared(psync_socket *api){
  pthread_mutex_lock(&sharedapi_mutex);
  if (!sharedapi)
    sharedapi=api;
  pthread_mutex_unlock(&sharedapi_mutex);
}

static void signal_all_waiters(){
  shared_api_waiter_t *waiter;
  while (!psync_list_isempty(&sharedapiwaiters)){
    waiter=psync_list_remove_head_element(&sharedapiwaiters, shared_api_waiter_t, list);
    waiter->api=(psync_socket *)-1;
    pthread_cond_signal(&waiter->cond);
  }
}

static void mark_shared_api_bad(psync_socket *api){
  pthread_mutex_lock(&sharedapi_mutex);
  if (sharedapi==api){
    sharedapi=NULL;
    signal_all_waiters();
  }
  pthread_mutex_unlock(&sharedapi_mutex);
}

static int pass_shared_api(psync_socket *api){
  shared_api_waiter_t *waiter;
  int ret;
  pthread_mutex_lock(&sharedapi_mutex);
  if (api!=sharedapi)
    ret=-1;
  else if (psync_list_isempty(&sharedapiwaiters)){
    ret=-1;
    sharedapi=NULL;
  }
  else{
    ret=0;
    waiter=psync_list_remove_head_element(&sharedapiwaiters, shared_api_waiter_t, list);
    waiter->api=api;
    pthread_cond_signal(&waiter->cond);
    debug(D_NOTICE, "passing shared api connection");
  }
  pthread_mutex_unlock(&sharedapi_mutex);
  return ret;
}

static psync_socket *get_shared_api(){
  pthread_mutex_lock(&sharedapi_mutex);
  if (sharedapi)
    return sharedapi; // not supposed to unlock, it will happen in wait_shared_api
  pthread_mutex_unlock(&sharedapi_mutex);
  return NULL;
}

static void release_bad_shared_api(psync_socket *api){
  if (sharedapi==api){
    sharedapi=NULL;
    signal_all_waiters();
  }
  pthread_mutex_unlock(&sharedapi_mutex);
}

static int wait_shared_api(){
  shared_api_waiter_t *waiter;
  psync_socket *capi;
  int ret;
  capi=sharedapi;
  waiter=psync_new(shared_api_waiter_t);
  pthread_cond_init(&waiter->cond, NULL);
  waiter->api=NULL;
  psync_list_add_tail(&sharedapiwaiters, &waiter->list);
  debug(D_NOTICE, "waiting for shared API connection");
  do {
    pthread_cond_wait(&waiter->cond, &sharedapi_mutex);
  } while (!waiter->api);
  if (waiter->api!=capi){
    assertw(waiter->api==(psync_socket *)-1);
    ret=-1;
  }
  else{
    debug(D_NOTICE, "waited for shared API connection");
    ret=0;
  }
  pthread_mutex_unlock(&sharedapi_mutex);
  pthread_cond_destroy(&waiter->cond);
  psync_free(waiter);
  return ret;
}

static void set_urls(psync_urls_t *urls, binresult *res){
  pthread_mutex_lock(&url_cache_mutex);
  if (res){
    urls->status=1;
    urls->urls=res;
    if (urls->refcnt++>0)
      pthread_cond_broadcast(&url_cache_cond);
  }
  else{
    psync_tree_del(&url_cache_tree, &urls->tree);
    if (urls->refcnt){
      urls->status=2;
      pthread_cond_broadcast(&url_cache_cond);
    }
    else
      psync_free(urls);
  }
  pthread_mutex_unlock(&url_cache_mutex);
}

static int get_urls(psync_request_t *request, psync_urls_t *urls){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("fileid", request->fileid), P_NUM("hash", request->hash), 
                    P_STR("timeformat", "timestamp"), P_BOOL("skipfilename", 1)};
  psync_socket *api;
  binresult *ret;
  psync_request_range_t *range;
  psync_list *l1, *l2;
  const binresult *hosts;
  unsigned long result;
  int tries;
  debug(D_NOTICE, "getting file URLs of fileid %lu, hash %lu together with requests", (unsigned long)request->fileid, (unsigned long)request->hash);
  tries=0;
  while (tries++<=5){
    api=psync_apipool_get();
    if (unlikely_log(!api))
      continue;
    if (unlikely(send_command_no_res(api, "getfilelink", params)!=PTR_OK))
      goto err1;
    psync_list_for_each_element(range, &request->ranges, psync_request_range_t, list){
      debug(D_NOTICE, "sending request for offset %lu, size %lu to API", (unsigned long)range->offset, (unsigned long)range->length);
      if (unlikely(psync_api_send_read_request(api, request->fileid, request->hash, range->offset, range->length)))
        goto err1;
    }
    mark_api_shared(api);
    ret=get_result_thread(api);
    if (unlikely_log(!ret)){
      mark_shared_api_bad(api);
      goto err1;
    }
    result=psync_find_result(ret, "result", PARAM_NUM)->num;
    if (unlikely(result!=0)){
      debug(D_WARNING, "getfilelink returned error %lu", result);
      psync_free(ret);
      mark_shared_api_bad(api);
      psync_apipool_release_bad(api);
      break;
    }
    hosts=psync_find_result(ret, "hosts", PARAM_ARRAY);
    debug(D_NOTICE, "got file URLs of fileid %lu, hash %lu", (unsigned long)request->fileid, (unsigned long)request->hash);
    if (likely_log(hosts->length && hosts->array[0]->type==PARAM_STR))
      psync_http_connect_and_cache_host(hosts->array[0]->str);
    /*if (of->initialsize>=PSYNC_FS_FILESIZE_FOR_2CONN && hosts->length>1 && hosts->array[1]->type==PARAM_STR)
      psync_http_connect_and_cache_host(hosts->array[1]->str);*/
    set_urls(urls, ret);
    psync_list_for_each_safe(l1, l2, &request->ranges){
      range=psync_list_element(l1, psync_request_range_t, list);
      if (psync_pagecache_read_range_from_api(request, range, api)){
        mark_shared_api_bad(api);
        psync_apipool_release_bad(api);
        return 0;
      }
      psync_list_del(l1);
      debug(D_NOTICE, "request for offset %lu, size %lu read from API", (unsigned long)range->offset, (unsigned long)range->length);
      psync_free(range);
    }
    if (pass_shared_api(api))
      psync_apipool_release(api);
    return 0;
err1:
    psync_apipool_release_bad(api);
  }
  return -1;
}

static psync_urls_t *get_urls_for_request(psync_request_t *req){
  char buff[32];
  psync_tree *el, **pel;
  psync_urls_t *urls;
  binresult *res;
  int64_t d;
  pthread_mutex_lock(&url_cache_mutex);
  el=url_cache_tree;
  pel=&url_cache_tree;
  d=-1;
  while (el){
    urls=psync_tree_element(el, psync_urls_t, tree);
    d=req->hash-urls->hash;
    if (d==0)
      break;
    else if (d<0){
      if (el->left)
        el=el->left;
      else{
        pel=&el->left;
        break;
      }
    }
    else{
      if (el->right)
        el=el->right;
      else{
        pel=&el->right;
        break;
      }
    }
  }
  if (d==0){
    urls->refcnt++;
    while (urls->status==0)
      pthread_cond_wait(&url_cache_cond, &url_cache_mutex);
    if (likely(urls->status==1)){
      pthread_mutex_unlock(&url_cache_mutex);
      return urls;
    }
    if (--urls->refcnt==0)
      psync_free(urls);
    pthread_mutex_unlock(&url_cache_mutex);
    return NULL;
  }
  urls=psync_new(psync_urls_t);
  urls->hash=req->hash;
  urls->refcnt=0;
  urls->status=0;
  *pel=&urls->tree;
  psync_tree_added_at(&url_cache_tree, el, &urls->tree);
  pthread_mutex_unlock(&url_cache_mutex);
  sprintf(buff, "URLS%"PRIu64, req->hash);
  res=(binresult *)psync_cache_get(buff);
  if (res){
    set_urls(urls, res);
    return urls;
  }
  if (get_urls(req, urls)){
    set_urls(urls, NULL);
    return NULL;
  }
  else
    return urls;
}

static void release_urls(psync_urls_t *urls){
  pthread_mutex_lock(&url_cache_mutex);
  if (--urls->refcnt==0){
    if (likely(urls->status==1)){
      char buff[32];
      time_t ctime, etime;
      psync_tree_del(&url_cache_tree, &urls->tree);
      ctime=psync_timer_time();
      etime=psync_find_result(urls->urls, "expires", PARAM_NUM)->num;
      if (etime>ctime+3600){
        sprintf(buff, "URLS%"PRIu64, urls->hash);
        psync_cache_add(buff, urls->urls, etime-ctime-3600, psync_free, 2);
        urls->urls=NULL;
      }
    }
    pthread_mutex_unlock(&url_cache_mutex);
    psync_free(urls->urls);
    psync_free(urls);
    return;
  }
  pthread_mutex_unlock(&url_cache_mutex);
}

static void release_bad_urls(psync_urls_t *urls){
  pthread_mutex_lock(&url_cache_mutex);
  if (urls->status==1){
    urls->status=2;
    psync_tree_del(&url_cache_tree, &urls->tree);
  }
  if (--urls->refcnt)
    urls=NULL;
  pthread_mutex_unlock(&url_cache_mutex);
  if (urls){
    psync_free(urls->urls);
    psync_free(urls);
  }
}

static uint64_t offset_round_down_to_page(uint64_t offset){
  return offset&~(((uint64_t)PSYNC_FS_PAGE_SIZE)-1);
}

static uint64_t size_round_up_to_page(uint64_t size){
  return ((size-1)|(((uint64_t)PSYNC_FS_PAGE_SIZE)-1))+1;
}

static int has_page_in_cache_by_hash(uint64_t hash, uint64_t pageid){
  psync_cache_page_t *page;
  psync_uint_t h;
  h=pagehash_by_hash_and_pageid(hash, pageid);
  pthread_mutex_lock(&cache_mutex);
  psync_list_for_each_element(page, &cache_hash[h], psync_cache_page_t, list)
    if (page->type==PAGE_TYPE_READ && page->hash==hash && page->pageid==pageid){
      pthread_mutex_unlock(&cache_mutex);
      return 1;
    }
  pthread_mutex_unlock(&cache_mutex);
  return 0;
}

static unsigned char *has_pages_in_db(uint64_t hash, uint64_t pageid, uint32_t pagecnt, int readahead){
  psync_sql_res *res;
  psync_uint_row row;
  unsigned char *ret;
  uint64_t fromid;
  uint32_t fcnt;
  ret=psync_new_cnt(unsigned char, pagecnt);
  memset(ret, 0, pagecnt);
  fromid=0;
  fcnt=0;
  res=psync_sql_query("SELECT pageid, id FROM pagecache WHERE type=+"NTO_STR(PAGE_TYPE_READ)" AND hash=? AND pageid>=? AND pageid<? ORDER BY pageid");
  psync_sql_bind_uint(res, 1, hash);
  psync_sql_bind_uint(res, 2, pageid);
  psync_sql_bind_uint(res, 3, pageid+pagecnt);
  while ((row=psync_sql_fetch_rowint(res))){
    ret[row[0]-pageid]=1;
    if (row[1]==fromid+fcnt)
      fcnt++;
    else{
      if (fcnt && readahead)
        psync_file_readahead(readcache, fromid*PSYNC_FS_PAGE_SIZE, fcnt*PSYNC_FS_PAGE_SIZE);
      fromid=row[1];
      fcnt=1;
    }
  }
  psync_sql_free_result(res);
  if (fcnt && readahead)
    psync_file_readahead(readcache, fromid*PSYNC_FS_PAGE_SIZE, fcnt*PSYNC_FS_PAGE_SIZE);
  return ret;
}

static int has_page_in_db(uint64_t hash, uint64_t pageid){
  psync_sql_res *res;
  psync_uint_row row;
  res=psync_sql_query("SELECT pageid FROM pagecache WHERE type=+"NTO_STR(PAGE_TYPE_READ)" AND hash=? AND pageid=?");
  psync_sql_bind_uint(res, 1, hash);
  psync_sql_bind_uint(res, 2, pageid);
  row=psync_sql_fetch_rowint(res);
  psync_sql_free_result(res);
  return row!=NULL;
}

static psync_int_t check_page_in_memory_by_hash(uint64_t hash, uint64_t pageid, char *buff, psync_uint_t size, psync_uint_t off){
  psync_cache_page_t *page;
  psync_uint_t h;
  psync_int_t ret;
  time_t tm;
  ret=-1;
  h=pagehash_by_hash_and_pageid(hash, pageid);
  pthread_mutex_lock(&cache_mutex);
  psync_list_for_each_element(page, &cache_hash[h], psync_cache_page_t, list)
    if (page->type==PAGE_TYPE_READ && page->hash==hash && page->pageid==pageid){
      tm=psync_timer_time();
      if (tm>page->lastuse+5){
        page->usecnt++;
        page->lastuse=tm;
      }
      if (size+off>page->size){
        if (off>page->size)
          size=0;
        else
          size=page->size-off;
      }
      memcpy(buff, page->page+off, size);
      ret=size;
    }
  pthread_mutex_unlock(&cache_mutex);
  return ret;
}

static int has_page_in_memory_by_hash(uint64_t hash, uint64_t pageid){
  psync_cache_page_t *page;
  psync_uint_t h;
  int ret;
  ret=0;
  h=pagehash_by_hash_and_pageid(hash, pageid);
  pthread_mutex_lock(&cache_mutex);
  psync_list_for_each_element(page, &cache_hash[h], psync_cache_page_t, list)
    if (page->type==PAGE_TYPE_READ && page->hash==hash && page->pageid==pageid){
      ret=1;
      break;
    }
  pthread_mutex_unlock(&cache_mutex);
  return ret;
}

typedef struct {
  time_t lastuse;
  uint32_t id;
  uint32_t usecnt;
} pagecache_entry;

static int pagecache_entry_cmp_lastuse(const void *p1, const void *p2){
  const pagecache_entry *e1, *e2;
  e1=(const pagecache_entry *)p1;
  e2=(const pagecache_entry *)p2;
  return (int)((int64_t)e1->lastuse-(int64_t)e2->lastuse);
}

static int pagecache_entry_cmp_usecnt_lastuse2(const void *p1, const void *p2){
  const pagecache_entry *e1, *e2;
  e1=(const pagecache_entry *)p1;
  e2=(const pagecache_entry *)p2;
  if (e1->usecnt>=2 && e2->usecnt<2)
    return 1;
  else if (e2->usecnt>=2 && e1->usecnt<2)
    return -1;
  else
    return (int)((int64_t)e1->lastuse-(int64_t)e2->lastuse);
}

static int pagecache_entry_cmp_usecnt_lastuse4(const void *p1, const void *p2){
  const pagecache_entry *e1, *e2;
  e1=(const pagecache_entry *)p1;
  e2=(const pagecache_entry *)p2;
  if (e1->usecnt>=4 && e2->usecnt<4)
    return 1;
  else if (e2->usecnt>=4 && e1->usecnt<4)
    return -1;
  else
    return (int)((int64_t)e1->lastuse-(int64_t)e2->lastuse);
}

static int pagecache_entry_cmp_usecnt_lastuse8(const void *p1, const void *p2){
  const pagecache_entry *e1, *e2;
  e1=(const pagecache_entry *)p1;
  e2=(const pagecache_entry *)p2;
  if (e1->usecnt>=8 && e2->usecnt<8)
    return 1;
  else if (e2->usecnt>=8 && e1->usecnt<8)
    return -1;
  else
    return (int)((int64_t)e1->lastuse-(int64_t)e2->lastuse);
}

static int pagecache_entry_cmp_usecnt_lastuse16(const void *p1, const void *p2){
  const pagecache_entry *e1, *e2;
  e1=(const pagecache_entry *)p1;
  e2=(const pagecache_entry *)p2;
  if (e1->usecnt>=16 && e2->usecnt<16)
    return 1;
  else if (e2->usecnt>=16 && e1->usecnt<16)
    return -1;
  else
    return (int)((int64_t)e1->lastuse-(int64_t)e2->lastuse);
}

/* sum should be around 90-95 percent, so after a run cache get smaller */
#define PSYNC_FS_CACHE_LRU_PERCENT 40
#define PSYNC_FS_CACHE_LRU2_PERCENT 25
#define PSYNC_FS_CACHE_LRU4_PERCENT 15
#define PSYNC_FS_CACHE_LRU8_PERCENT 10
#define PSYNC_FS_CACHE_LRU16_PERCENT 5

static void clean_cache(){
  psync_sql_res *res;
  uint64_t ocnt, cnt, i, j, e;
  psync_uint_row row;
  pagecache_entry *entries;
  debug(D_NOTICE, "cleaning cache, free cache pages %u", (unsigned)free_db_pages);
  if (pthread_mutex_trylock(&clean_cache_mutex)){
    debug(D_NOTICE, "cache clean already in progress, skipping");
    return;
  }
  while (clean_cache_stoppers){
    clean_cache_waiters++;
    pthread_cond_wait(&clean_cache_cond, &clean_cache_mutex);
    if (--clean_cache_waiters){
      // leave the last waiter to do the job
      pthread_mutex_unlock(&clean_cache_mutex);
      return;
    }
  }
  cnt=psync_sql_cellint("SELECT COUNT(*) FROM pagecache", 0);
  entries=(pagecache_entry *)psync_malloc(cnt*sizeof(pagecache_entry));
  i=0;
  while (i<cnt){
    res=psync_sql_query("SELECT id, lastuse, usecnt, type FROM pagecache WHERE id>? ORDER BY id LIMIT 10000");
    psync_sql_bind_uint(res, 1, i);
    while ((row=psync_sql_fetch_rowint(res))){
      if (i>=cnt)
        break;
      if (row[3]!=PAGE_TYPE_READ)
        continue;
      entries[i].lastuse=row[1];
      entries[i].id=row[0];
      entries[i].usecnt=row[2];
      i++;
    }
    psync_sql_free_result(res);
    psync_milisleep(1);
  }
  ocnt=cnt=i;
  debug(D_NOTICE, "read %lu entries", (unsigned long)cnt);
  qsort(entries, cnt, sizeof(pagecache_entry), pagecache_entry_cmp_lastuse);
  cnt-=PSYNC_FS_CACHE_LRU_PERCENT*ocnt/100;
  debug(D_NOTICE, "sorted entries by lastuse, continuing with %lu oldest entries", (unsigned long)cnt);
  qsort(entries, cnt, sizeof(pagecache_entry), pagecache_entry_cmp_usecnt_lastuse2);
  cnt-=PSYNC_FS_CACHE_LRU2_PERCENT*ocnt/100;
  debug(D_NOTICE, "sorted entries by more than 2 uses and lastuse, continuing with %lu entries", (unsigned long)cnt);
  qsort(entries, cnt, sizeof(pagecache_entry), pagecache_entry_cmp_usecnt_lastuse4);
  cnt-=PSYNC_FS_CACHE_LRU4_PERCENT*ocnt/100;
  debug(D_NOTICE, "sorted entries by more than 4 uses and lastuse, continuing with %lu entries", (unsigned long)cnt);
  qsort(entries, cnt, sizeof(pagecache_entry), pagecache_entry_cmp_usecnt_lastuse8);
  cnt-=PSYNC_FS_CACHE_LRU8_PERCENT*ocnt/100;
  debug(D_NOTICE, "sorted entries by more than 8 uses and lastuse, continuing with %lu entries", (unsigned long)cnt);
  qsort(entries, cnt, sizeof(pagecache_entry), pagecache_entry_cmp_usecnt_lastuse16);
  cnt-=PSYNC_FS_CACHE_LRU16_PERCENT*ocnt/100;
  debug(D_NOTICE, "sorted entries by more than 16 uses and lastuse, deleting %lu entries", (unsigned long)cnt);
  ocnt=(cnt+255)/256;
  for (j=0; j<ocnt; j++){
    i=j*256;
    e=i+256;
    if (e>cnt)
      e=cnt;
    psync_sql_start_transaction();
    res=psync_sql_prep_statement("UPDATE pagecache SET type="NTO_STR(PAGE_TYPE_FREE)", hash=NULL, pageid=NULL WHERE id=?");
    for (; i<e; i++){
      psync_sql_bind_uint(res, 1, entries[i].id);
      psync_sql_run(res);
      free_db_pages++;
    }
    psync_sql_free_result(res);
    psync_sql_commit_transaction();
    psync_milisleep(5);
  }
  pthread_mutex_unlock(&clean_cache_mutex);
  psync_free(entries);
  psync_sql_sync();
  debug(D_NOTICE, "finished cleaning cache, free cache pages %u", (unsigned)free_db_pages);
}

static int cmp_flush_pages(const psync_list *p1, const psync_list *p2){
  const psync_cache_page_t *page1, *page2;
  page1=psync_list_element(p1, const psync_cache_page_t, flushlist);
  page2=psync_list_element(p2, const psync_cache_page_t, flushlist);
  if (page1->hash<page2->hash)
    return -1;
  else if (page1->hash>page2->hash)
    return 1;
  else if (page1->pageid<page2->pageid)
    return -1;
  else if (page1->pageid>page2->pageid)
    return 1;
  else
    return 0;
}

static int cmp_discard_pages(const psync_list *p1, const psync_list *p2){
  const psync_cache_page_t *page1, *page2;
  page1=psync_list_element(p1, const psync_cache_page_t, flushlist);
  page2=psync_list_element(p2, const psync_cache_page_t, flushlist);
  if (page1->lastuse<page2->lastuse)
    return -1;
  else if (page1->lastuse>page2->lastuse)
    return 1;
  else
    return 0;
}

static int check_disk_full(){
  int64_t filesize, freespace;
  uint64_t minlocal, maxpage;
  psync_sql_res *res;
  db_cache_max_page=psync_sql_cellint("SELECT MAX(id) FROM pagecache", 0);
  filesize=psync_file_size(readcache);
  if (unlikely_log(filesize==-1) || filesize>=db_cache_max_page*PSYNC_FS_PAGE_SIZE)
    return 0;
  freespace=psync_get_free_space_by_path(psync_setting_get_string(_PS(fscachepath)));
  minlocal=psync_setting_get_uint(_PS(minlocalfreespace));
  if (unlikely_log(freespace==-1) || minlocal+db_cache_max_page*PSYNC_FS_PAGE_SIZE-filesize<=freespace){
    psync_set_local_full(0);
    return 0;
  }
  debug(D_NOTICE, "local disk is full, freespace=%lu, minfreespace=%lu", (unsigned long)freespace, (unsigned long)minlocal);
  psync_set_local_full(1);
  if (minlocal>=freespace)
    maxpage=filesize/PSYNC_FS_PAGE_SIZE;
  else
    maxpage=(filesize+freespace-minlocal)/PSYNC_FS_PAGE_SIZE;
  res=psync_sql_prep_statement("DELETE FROM pagecache WHERE id>?");
  psync_sql_bind_uint(res, 1, maxpage);
  psync_sql_run_free(res);
  free_db_pages=psync_sql_cellint("SELECT COUNT(*) FROM pagecache WHERE type="NTO_STR(PAGE_TYPE_FREE), 0);
  db_cache_max_page=maxpage;
  debug(D_NOTICE, "free_db_pages=%u, db_cache_max_page=%lu", (unsigned)free_db_pages, (unsigned long)db_cache_max_page);
  return 1;
}

static int flush_pages(int nosleep){
  static time_t lastflush=0;
  psync_sql_res *res;
  psync_uint_row row;
  psync_cache_page_t *page;
  psync_list pages_to_flush;
  psync_uint_t i, updates, pagecnt;
  time_t ctime;
  uint32_t cpih;
  int ret, diskfull;
  flushedbetweentimers=1;
  pthread_mutex_lock(&flush_cache_mutex);
  diskfull=check_disk_full();
  updates=0;
  pagecnt=0;
  ctime=psync_timer_time();
  psync_list_init(&pages_to_flush);
  pthread_mutex_lock(&cache_mutex);
  if (diskfull && psync_list_isempty(&free_pages) && free_db_pages==0){
    debug(D_NOTICE, "disk is full, discarding some pages");
    for (i=0; i<CACHE_HASH; i++)
      psync_list_for_each_element(page, &cache_hash[i], psync_cache_page_t, list)
        if (page->type==PAGE_TYPE_READ)
          psync_list_add_tail(&pages_to_flush, &page->flushlist);
    pthread_mutex_unlock(&cache_mutex);
    psync_list_sort(&pages_to_flush, cmp_discard_pages);
    pthread_mutex_lock(&cache_mutex);
    i=0;
    psync_list_for_each_element(page, &pages_to_flush, psync_cache_page_t, flushlist){
      psync_list_del(&page->list);
      psync_list_add_head(&free_pages, &page->list);
      cache_pages_in_hash--;
      if (++i>=CACHE_PAGES/2)
        break;
    }
    debug(D_NOTICE, "discarded %u pages", (unsigned)i);
    psync_list_init(&pages_to_flush);
  }
  if (cache_pages_in_hash){
    debug(D_NOTICE, "flushing cache");
    for (i=0; i<CACHE_HASH; i++)
      psync_list_for_each_element(page, &cache_hash[i], psync_cache_page_t, list)
        if (page->type==PAGE_TYPE_READ){
          psync_list_add_tail(&pages_to_flush, &page->flushlist);
          pagecnt++;
        }
    cache_pages_in_hash=pagecnt;
    pthread_mutex_unlock(&cache_mutex);
    debug(D_NOTICE, "cache_pages_in_hash=%u", (unsigned)pagecnt);
    psync_list_sort(&pages_to_flush, cmp_flush_pages);
    res=psync_sql_query("SELECT id FROM pagecache WHERE type="NTO_STR(PAGE_TYPE_FREE)" ORDER BY id LIMIT ?");
    psync_sql_bind_uint(res, 1, pagecnt);
    psync_list_for_each_element(page, &pages_to_flush, psync_cache_page_t, flushlist){
      if (unlikely(!(row=psync_sql_fetch_rowint(res)))){
        psync_list *l1, *l2;
        l1=&page->flushlist;
        do{
          l2=l1->next;
          psync_list_del(l1);
          l1=l2;
        } while (l1!=&pages_to_flush);
        break;
      }
      page->flushpageid=row[0];
    }
    psync_sql_free_result(res);
/*    res=psync_sql_query("SELECT id FROM pagecache WHERE type="NTO_STR(PAGE_TYPE_FREE)" ORDER BY id");
    for (i=0; i<CACHE_HASH; i++)
      psync_list_for_each_element(page, &cache_hash[i], psync_cache_page_t, list)
        if (page->type==PAGE_TYPE_READ){
          if (!(row=psync_sql_fetch_rowint(res)))
            goto break2;
          page->flushpageid=row[0];
          psync_list_add_tail(&pages_to_flush, &page->flushlist);
        }
break2:
    psync_sql_free_result(res);
    pthread_mutex_unlock(&cache_mutex);*/
    i=0;
    psync_list_for_each_element(page, &pages_to_flush, psync_cache_page_t, flushlist){
      if (psync_file_pwrite(readcache, page->page, PSYNC_FS_PAGE_SIZE, (uint64_t)page->flushpageid*PSYNC_FS_PAGE_SIZE)!=PSYNC_FS_PAGE_SIZE){
        debug(D_ERROR, "write to cache file failed");
        pthread_mutex_unlock(&flush_cache_mutex);
        return -1;
      }
      i++;
    }
    debug(D_NOTICE, "cache data of %u pages written", (unsigned)i);
    /* if we can afford it, wait a while before calling fsync() as at least on Linux this blocks reads from the same file until it returns */
    if (!nosleep){
      i=0;
      pthread_mutex_lock(&cache_mutex);
      while (cache_pages_free>=CACHE_PAGES*5/100 && i++<200){
        pthread_mutex_unlock(&cache_mutex);
        psync_milisleep(10);
        pthread_mutex_lock(&cache_mutex);
      }
      pthread_mutex_unlock(&cache_mutex);
    }
    if (psync_file_sync(readcache)){
      debug(D_ERROR, "flush of cache file failed");
      pthread_mutex_unlock(&flush_cache_mutex);
      return -1;
    }
    debug(D_NOTICE, "cache data synced");
    pthread_mutex_lock(&cache_mutex);
  }  
  psync_sql_start_transaction();
  if (db_cache_max_page<db_cache_in_pages && cache_pages_in_hash && !diskfull){
    i=0;
    res=psync_sql_prep_statement("INSERT INTO pagecache (type) VALUES ("NTO_STR(PAGE_TYPE_FREE)")");
    while (db_cache_max_page+i<db_cache_in_pages && i<CACHE_PAGES && i<cache_pages_in_hash){
      psync_sql_run(res);
      i++;
    }
    psync_sql_free_result(res);
    free_db_pages+=i;
    db_cache_max_page+=i;
    debug(D_NOTICE, "inserted %lu new free pages to database, db_cache_in_pages=%lu, db_cache_max_page=%lu", 
                    (unsigned long)i, (unsigned long)db_cache_in_pages, (unsigned long)db_cache_max_page);
    updates++;
  }
  cpih=cache_pages_in_hash;
  if (!psync_list_isempty(&pages_to_flush)){
    pagecnt=0;
    res=psync_sql_prep_statement("UPDATE OR IGNORE pagecache SET hash=?, pageid=?, type="NTO_STR(PAGE_TYPE_READ)", lastuse=?, usecnt=?, size=? WHERE id=?");
    psync_list_for_each_element(page, &pages_to_flush, psync_cache_page_t, flushlist){
      psync_list_del(&page->list);
      psync_sql_bind_uint(res, 1, page->hash);
      psync_sql_bind_uint(res, 2, page->pageid);
      psync_sql_bind_uint(res, 3, page->lastuse);
      psync_sql_bind_uint(res, 4, page->usecnt);
      psync_sql_bind_uint(res, 5, page->size);
      psync_sql_bind_uint(res, 6, page->flushpageid);
      psync_sql_run(res);
      cache_pages_free++;
      if (likely(psync_sql_affected_rows())){
        updates++;
        pagecnt++;
        free_db_pages--;
      }
      psync_list_add_head(&free_pages, &page->list);
      if (updates%64==0){
        psync_sql_free_result(res);
        psync_sql_commit_transaction();
        pthread_mutex_unlock(&cache_mutex);
        psync_milisleep(1);
        pthread_mutex_lock(&cache_mutex);
        psync_sql_start_transaction();
        res=psync_sql_prep_statement("UPDATE OR IGNORE pagecache SET hash=?, pageid=?, type="NTO_STR(PAGE_TYPE_READ)", lastuse=?, usecnt=?, size=? WHERE id=?");
      }
    }
    psync_sql_free_result(res);
    debug(D_NOTICE, "flushed %u pages to cache file, free db pages %u, cache_pages_in_hash=%u", (unsigned)pagecnt,
          (unsigned)free_db_pages, (unsigned)cache_pages_in_hash);
    cache_pages_in_hash-=pagecnt;
  }
  if (cachepages_to_update_cnt && (cpih || cachepages_to_update_cnt>=DB_CACHE_UPDATE_HASH/4 || lastflush+300<ctime)){
    res=psync_sql_prep_statement("UPDATE pagecache SET lastuse=?, usecnt=usecnt+? WHERE id=?");
    for (i=0; i<DB_CACHE_UPDATE_HASH; i++)
      if (cachepages_to_update[i].pagecacheid){
        psync_sql_bind_uint(res, 1, cachepages_to_update[i].lastuse);
        psync_sql_bind_uint(res, 2, cachepages_to_update[i].usecnt);
        psync_sql_bind_uint(res, 3, cachepages_to_update[i].pagecacheid);
        psync_sql_run(res);
        memset(&cachepages_to_update[i], 0, sizeof(psync_cachepage_to_update));
        updates++;
        if (updates%128==0){
          psync_sql_free_result(res);
          psync_sql_commit_transaction();
          pthread_mutex_unlock(&cache_mutex);
          psync_milisleep(1);
          pthread_mutex_lock(&cache_mutex);
          psync_sql_start_transaction();
          res=psync_sql_prep_statement("UPDATE pagecache SET lastuse=?, usecnt=usecnt+? WHERE id=?");
        }
      }
    psync_sql_free_result(res);
    debug(D_NOTICE, "flushed %u access records to database", (unsigned)cachepages_to_update_cnt);
    cachepages_to_update_cnt=0;
    lastflush=ctime;
  }
  flushchacherun=0;
  if (updates){
    ret=psync_sql_commit_transaction();
    pthread_mutex_unlock(&cache_mutex);
    pthread_mutex_unlock(&flush_cache_mutex);
    if (free_db_pages<=CACHE_PAGES*2)
      psync_run_thread("clean cache", clean_cache);
    return ret;
  }
  else{
    psync_sql_rollback_transaction();
    pthread_mutex_unlock(&cache_mutex);
    pthread_mutex_unlock(&flush_cache_mutex);
    return 0;
  }
}

int psync_pagecache_flush(){
  if (flush_pages(1))
    return -EIO;
  else
    return 0;
}

static void psync_pagecache_flush_timer(psync_timer_t timer, void *ptr){
  if (!flushedbetweentimers && (cache_pages_in_hash || cachepages_to_update_cnt))
    psync_run_thread("flush pages timer", flush_pages_noret);
  flushedbetweentimers=0;
}

static void mark_pagecache_used(uint64_t pagecacheid){
  uint64_t h;
  time_t tm;
  if (cachepages_to_update_cnt>DB_CACHE_UPDATE_HASH/2)
    flush_pages(1);
  h=pagecacheid%DB_CACHE_UPDATE_HASH;
  tm=psync_timer_time();
  pthread_mutex_lock(&cache_mutex);
  while (1){
    if (cachepages_to_update[h].pagecacheid==0){
      cachepages_to_update[h].pagecacheid=pagecacheid;
      cachepages_to_update[h].lastuse=tm;
      cachepages_to_update[h].usecnt=1;
      cachepages_to_update_cnt++;
      break;
    }
    else if (cachepages_to_update[h].pagecacheid==pagecacheid){
      if (tm>cachepages_to_update[h].lastuse+5){
        cachepages_to_update[h].lastuse=tm;
        cachepages_to_update[h].usecnt++;
      }
      break;
    }
    if (++h>=DB_CACHE_UPDATE_HASH)
      h=0;
  }
  pthread_mutex_unlock(&cache_mutex);
}

static psync_int_t check_page_in_database_by_hash(uint64_t hash, uint64_t pageid, char *buff, psync_uint_t size, psync_uint_t off){
  psync_sql_res *res;
  psync_variant_row row;
  size_t dsize;
  ssize_t readret;
  psync_int_t ret;
  uint64_t pagecacheid;
  ret=-1;
  res=psync_sql_query("SELECT id, size FROM pagecache WHERE type="NTO_STR(PAGE_TYPE_READ)" AND hash=? AND pageid=?");
  psync_sql_bind_uint(res, 1, hash);
  psync_sql_bind_uint(res, 2, pageid);
  if ((row=psync_sql_fetch_row(res))){
    pagecacheid=psync_get_number(row[0]);
    dsize=psync_get_number(row[1]);
    if (size+off>dsize){
      if (off>dsize)
        size=0;
      else
        size=dsize-off;
    }
    ret=size;
  }
  psync_sql_free_result(res);
  if (ret!=-1){
    readret=psync_file_pread(readcache, buff, size, pagecacheid*PSYNC_FS_PAGE_SIZE+off);
    if (readret!=size){
      debug(D_ERROR, "failed to read %lu bytes from cache file at offset %lu, read returned %ld, errno=%ld",
            (unsigned long)size, (unsigned long)(pagecacheid*PSYNC_FS_PAGE_SIZE+off), (long)readret, (long)psync_fs_err());
      res=psync_sql_prep_statement("UPDATE pagecache SET type="NTO_STR(PAGE_TYPE_FREE)", pageid=NULL, hash=NULL WHERE id=?");
      psync_sql_bind_uint(res, 1, pagecacheid);
      psync_sql_run_free(res);
      ret=-1;
    }
    else
      mark_pagecache_used(pagecacheid);
  }
  return ret;
}

int psync_pagecache_read_modified_locked(psync_openfile_t *of, char *buf, uint64_t size, uint64_t offset){
  psync_interval_tree_t *fi;
  uint64_t isize, ioffset;
  ssize_t br;
  int rd;
  fi=psync_interval_tree_first_interval_containing_or_after(of->writeintervals, offset);
  if (fi && fi->from<=offset && fi->to>=offset+size){
    debug(D_NOTICE, "reading %lu bytes at offset %lu only from local storage", (unsigned long)size, (unsigned long)offset);
    br=psync_file_pread(of->datafile, buf, size, offset);
    pthread_mutex_unlock(&of->mutex);
    if (br==-1)
      return -EIO;
    else
      return br;
  }
  rd=psync_pagecache_read_unmodified_locked(of, buf, size, offset);
  if (rd<0)
    return rd;
  pthread_mutex_lock(&of->mutex);
  fi=psync_interval_tree_first_interval_containing_or_after(of->writeintervals, offset);
  if (!fi || fi->from>=offset+size){
    pthread_mutex_unlock(&of->mutex);
    if (fi)
      br=fi->from;
    else
      br=-1;
    debug(D_NOTICE, "reading %lu bytes at offset %lu only from remote fileid %lu revision %lu, read returned %d, next local interval starts at %ld", 
          (unsigned long)size, (unsigned long)offset, (unsigned long)of->remotefileid, (unsigned long)of->hash, rd, (long)br);
    return rd;
  }
  debug(D_NOTICE, "reading %lu bytes at offset %lu from both network and local", (unsigned long)size, (unsigned long)offset);
  do {
    ioffset=fi->from;
    isize=fi->to-fi->from;
    if (ioffset<offset){
      isize-=offset-ioffset;
      ioffset=offset;
    }
    if (ioffset+isize>offset+size)
      isize=offset+size-ioffset;
    debug(D_NOTICE, "reading %lu bytes at offset %lu from local storage", (unsigned long)isize, (unsigned long)ioffset);
    br=psync_file_pread(of->datafile, buf+ioffset-offset, isize, ioffset);
    if (br==-1){
      pthread_mutex_unlock(&of->mutex);
      return -EIO;
    }
    if (rd!=size && br+ioffset-offset>rd)
      rd=br+ioffset-offset;
    fi=psync_interval_tree_get_next(fi);
  } while (fi && fi->from<offset+size);
  pthread_mutex_unlock(&of->mutex);
  return rd;
}

/*#define run_command_get_res(cmd, params) do_run_command_get_res(cmd, strlen(cmd), params, sizeof(params)/sizeof(binparam))

static binresult *do_run_command_get_res(const char *cmd, size_t cmdlen, const binparam *params, size_t paramscnt){
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
    psync_timer_notify_exception();
    goto neterr;
  }
  result=psync_find_result(res, "result", PARAM_NUM)->num;
  if (result){
    debug(D_WARNING, "command %s returned code %u", cmd, (unsigned)result);
    psync_free(res);
    return NULL;
  }
  else
    return res;
neterr:
  return NULL;
}*/

static void psync_pagecache_free_request(psync_request_t *request){
  psync_list_for_each_element_call(&request->ranges, psync_request_range_t, list, psync_free);
  psync_free(request);
}

static void psync_pagecache_send_error_page_wait(psync_page_wait_t *pw, int err){
  psync_page_waiter_t *pwt;
  psync_list_del(&pw->list);
  psync_list_for_each_element(pwt, &pw->waiters, psync_page_waiter_t, listpage){
    pwt->error=err;
    pwt->ready=1;
    pthread_cond_broadcast(&pwt->cond);
  }
  psync_free(pw);
}

static void psync_pagecache_send_range_error(psync_request_range_t *range, psync_request_t *request, int err){
  uint64_t first_page_id;
  psync_page_wait_t *pw;
  psync_uint_t len, i, h;
  first_page_id=range->offset/PSYNC_FS_PAGE_SIZE;
  len=range->length/PSYNC_FS_PAGE_SIZE;
  debug(D_NOTICE, "sending error %d to request for offset %lu, length %lu of fileid %lu hash %lu",
                  err, (unsigned long)range->offset, (unsigned long)range->length, (unsigned long)request->fileid, (unsigned long)request->hash);
  for (i=0; i<len; i++){
    h=waiterhash_by_hash_and_pageid(request->of->hash, first_page_id+i);
    psync_list_for_each_element(pw, &wait_page_hash[h], psync_page_wait_t, list)
      if (pw->hash==request->of->hash && pw->pageid==first_page_id+i){
        psync_pagecache_send_error_page_wait(pw, err);
        break;
      }
  }
}

static void psync_pagecache_send_error(psync_request_t *request, int err){
  psync_request_range_t *range;
  lock_wait(request->of->hash);
  psync_list_for_each_element(range, &request->ranges, psync_request_range_t, list)
    psync_pagecache_send_range_error(range, request, err);
  unlock_wait(request->of->hash);
  psync_fs_dec_of_refcnt_and_readers(request->of);
  psync_pagecache_free_request(request);
}

static int psync_pagecache_read_range_from_sock(psync_request_t *request, psync_request_range_t *range, psync_http_socket *sock){
  uint64_t first_page_id;
  psync_page_wait_t *pw;
  psync_cache_page_t *page;
  psync_uint_t len, i, h;
  int rb;
  first_page_id=range->offset/PSYNC_FS_PAGE_SIZE;
  len=range->length/PSYNC_FS_PAGE_SIZE;
  rb=psync_http_next_request(sock);
  if (unlikely(rb)){
    if (rb==410 || rb==404 || rb==-1){
      debug(D_WARNING, "got %d from psync_http_next_request, freeing URLs and requesting retry", rb);
      return 1;
    }
    else{
      debug(D_WARNING, "got %d from psync_http_next_request, returning error", rb);
      return -1;
    }
  }
  for (i=0; i<len; i++){
    page=psync_pagecache_get_free_page();
    rb=psync_http_request_readall(sock, page->page, PSYNC_FS_PAGE_SIZE);
    if (unlikely_log(rb<=0)){
      psync_pagecache_return_free_page(page);
      psync_timer_notify_exception();
      return -1;
    }
    page->hash=request->of->hash;
    page->pageid=first_page_id+i;
    page->lastuse=psync_timer_time();
    page->size=rb;
    page->usecnt=0;
    page->type=PAGE_TYPE_READ;
    h=waiterhash_by_hash_and_pageid(page->hash, page->pageid);
    lock_wait(page->hash);
    psync_list_for_each_element(pw, &wait_page_hash[h], psync_page_wait_t, list)
      if (pw->hash==page->hash && pw->pageid==page->pageid){
        psync_pagecache_send_page_wait_page(pw, page);
        break;
      }
    unlock_wait(page->hash);
    pthread_mutex_lock(&cache_mutex);
    psync_list_add_tail(&cache_hash[pagehash_by_hash_and_pageid(page->hash, page->pageid)], &page->list);
    cache_pages_in_hash++;
    pthread_mutex_unlock(&cache_mutex);
  }
  return 0;
}

static void psync_pagecache_read_unmodified_thread(void *ptr){
  psync_request_t *request;
  psync_http_socket *sock;
  psync_socket *api;
  const char *host;
  const char *path;
  psync_request_range_t *range;
  const binresult *hosts;
  psync_urls_t *urls;
  int err, tries;
  request=(psync_request_t *)ptr;
  if (psync_status_get(PSTATUS_TYPE_ONLINE)==PSTATUS_ONLINE_OFFLINE){
    psync_pagecache_send_error(request, -ENOTCONN);
    return;
  }
  range=psync_list_element(request->ranges.next, psync_request_range_t, list);
  debug(D_NOTICE, "thread run, first offset %lu, size %lu", (unsigned long)range->offset, (unsigned long)range->length);
  tries=0;
retry:
  if (!(urls=get_urls_for_request(request))){
    psync_pagecache_send_error(request, -EIO);
    return;
  }
  if (psync_list_isempty(&request->ranges)){
    release_urls(urls);
    psync_fs_dec_of_refcnt_and_readers(request->of);
    psync_pagecache_free_request(request);
    return;
  }
  hosts=psync_find_result(urls->urls, "hosts", PARAM_ARRAY);
  sock=psync_http_connect_multihost_from_cache(hosts, &host);
  if (!sock){
    if ((api=psync_apipool_get_from_cache())){
      debug(D_NOTICE, "no cached server connections, but got cached API connection, serving request from API");
      if (likely_log(hosts->length && hosts->array[0]->type==PARAM_STR))
        psync_http_connect_and_cache_host(hosts->array[0]->str);
      psync_list_for_each_element(range, &request->ranges, psync_request_range_t, list){
        debug(D_NOTICE, "sending request for offset %lu, size %lu to API", (unsigned long)range->offset, (unsigned long)range->length);
        if (psync_api_send_read_request(api, request->fileid, request->hash, range->offset, range->length))
          goto err_api1;
      }
      mark_api_shared(api);
      psync_list_for_each_element(range, &request->ranges, psync_request_range_t, list)
        if ((err=psync_pagecache_read_range_from_api(request, range, api))){
          mark_shared_api_bad(api);
          if (err==-2 && psync_list_is_head(&request->ranges, &range->list))
            goto err_api1;
          else{
            psync_apipool_release_bad(api);
            goto err0;
          }
        }
      if (pass_shared_api(api))
        psync_apipool_release(api);
      debug(D_NOTICE, "request from API finished");
      goto ok1;
err_api1:
      psync_apipool_release_bad(api);
      debug(D_WARNING, "error reading range from API, trying from content servers");
    }
    else if ((api=get_shared_api())){
      debug(D_NOTICE, "no cached server connections, no cached API servers, but got shared API connection sending request to shared API");
      psync_list_for_each_element(range, &request->ranges, psync_request_range_t, list){
        debug(D_NOTICE, "sending request for offset %lu, size %lu to shared API", (unsigned long)range->offset, (unsigned long)range->length);
        if (psync_api_send_read_request_thread(api, request->fileid, request->hash, range->offset, range->length))
          goto err_api2;
      }
      if (wait_shared_api())
        goto err_api0;
      psync_list_for_each_element(range, &request->ranges, psync_request_range_t, list)
        if ((err=psync_pagecache_read_range_from_api(request, range, api))){
          mark_shared_api_bad(api);
          if (err==-2 && psync_list_is_head(&request->ranges, &range->list))
            goto err_api0;
          else{
            psync_apipool_release_bad(api);
            goto err0;
          }
        }
      if (pass_shared_api(api))
        psync_apipool_release(api);
      debug(D_NOTICE, "request from shared API finished");
      goto ok1;
err_api2:
      release_bad_shared_api(api);
err_api0:
      debug(D_WARNING, "error reading range from API, trying from content servers");
    }
  }
  if (!sock)
    sock=psync_http_connect_multihost(hosts, &host);
  if (unlikely_log(!sock))
    goto err0;
//  debug(D_NOTICE, "connected to %s", host);
  path=psync_find_result(urls->urls, "path", PARAM_STR)->str;
  psync_list_for_each_element(range, &request->ranges, psync_request_range_t, list){
    debug(D_NOTICE, "sending request for offset %lu, size %lu", (unsigned long)range->offset, (unsigned long)range->length);
    if (psync_http_request(sock, host, path, range->offset, range->offset+range->length-1)){
      if (tries++<5){
        psync_http_close(sock);
        goto retry;
      }
      else
        goto err1;
    }
  }
  psync_list_for_each_element(range, &request->ranges, psync_request_range_t, list)
    if ((err=psync_pagecache_read_range_from_sock(request, range, sock))){
      if (err==1 && tries++<5){
        psync_http_close(sock);
        release_bad_urls(urls);
        goto retry;
      }
      else
        goto err1;
    }
  psync_http_close(sock);
  debug(D_NOTICE, "request from %s finished", host);
ok1:
  psync_fs_dec_of_refcnt_and_readers(request->of);
  psync_pagecache_free_request(request);
  release_urls(urls);
  return;
err1:
  psync_http_close(sock);
err0:
  psync_pagecache_send_error(request, -EIO);
  release_urls(urls);
  return;
}

static void psync_pagecache_read_unmodified_readahead(psync_openfile_t *of, uint64_t offset, uint64_t size, psync_list *ranges, psync_request_range_t *range,
                                                      psync_fileid_t fileid, uint64_t hash, uint64_t initialsize){
  uint64_t readahead, frompageoff, topageoff, first_page_id, rto;
  psync_int_t i, pagecnt, h, streamid;
  psync_page_wait_t *pw;
  time_t ctime;
  unsigned char *pages_in_db;
  int found;
  if (offset+size>=initialsize)
    return;
  readahead=0;
  frompageoff=offset/PSYNC_FS_PAGE_SIZE;
  topageoff=((offset+size+PSYNC_FS_PAGE_SIZE-1)/PSYNC_FS_PAGE_SIZE)-1;
  ctime=psync_timer_time();
  found=0;
  for (streamid=0; streamid<PSYNC_FS_FILESTREAMS_CNT; streamid++)
    if (of->streams[streamid].frompage<=frompageoff && of->streams[streamid].topage+2>=frompageoff){
      of->streams[streamid].id=++of->laststreamid;
      readahead=of->streams[streamid].length;
      of->streams[streamid].frompage=frompageoff;
      of->streams[streamid].topage=topageoff;
      of->streams[streamid].length+=size;
      of->streams[streamid].lastuse=ctime;
      break;
    }
    else if (of->streams[streamid].lastuse>=ctime-2)
      found++;
  if (streamid==PSYNC_FS_FILESTREAMS_CNT){
    uint64_t min;
    debug(D_NOTICE, "ran out of readahead streams");
    min=~(uint64_t)0;
    streamid=0;
    for (i=0; i<PSYNC_FS_FILESTREAMS_CNT; i++)
      if (of->streams[i].id<min){
        min=of->streams[i].id;
        streamid=i;
      }
    of->streams[streamid].id=++of->laststreamid;
    of->streams[streamid].frompage=frompageoff;
    of->streams[streamid].topage=topageoff;
    of->streams[streamid].length=size;
    of->streams[streamid].requestedto=0;
    of->streams[streamid].lastuse=ctime;
    if (found==1 && of->currentspeed*4>readahead && range){
      debug(D_NOTICE, "found just one freshly used stream, increasing readahead to four times current speed %u", (unsigned int)of->currentspeed*4);
      readahead=size_round_up_to_page(of->currentspeed*4);
    }
  }
  if (of->runningreads>=6 && !range)
    return;
  if (offset==0 && (size<PSYNC_FS_MIN_READAHEAD_START) && readahead<PSYNC_FS_MIN_READAHEAD_START-size)
    readahead=PSYNC_FS_MIN_READAHEAD_START-size;
  else if (offset==PSYNC_FS_MIN_READAHEAD_START/2 && readahead==PSYNC_FS_MIN_READAHEAD_START/2){
    of->streams[streamid].length+=offset;
    readahead=(PSYNC_FS_MIN_READAHEAD_START/2)*3;
  }
  else if (offset!=0 && (size<PSYNC_FS_MIN_READAHEAD_RAND) && readahead<PSYNC_FS_MIN_READAHEAD_RAND-size)
    readahead=PSYNC_FS_MIN_READAHEAD_RAND-size;
  if (readahead>PSYNC_FS_MAX_READAHEAD)
    readahead=PSYNC_FS_MAX_READAHEAD;
  if (of->currentspeed*PSYNC_FS_MAX_READAHEAD_SEC>PSYNC_FS_MIN_READAHEAD_START && readahead>of->currentspeed*PSYNC_FS_MAX_READAHEAD_SEC)
    readahead=size_round_up_to_page(of->currentspeed*PSYNC_FS_MAX_READAHEAD_SEC);
  if (!range){
    if (readahead>=8192*1024)
      readahead=(readahead+offset+size)/(4*1024*1024)*(4*1024*1024)-offset-size;
    else if (readahead>=2048*1024)
      readahead=(readahead+offset+size)/(1024*1024)*(1024*1024)-offset-size;
    else if (readahead>=512*1024)
      readahead=(readahead+offset+size)/(256*1024)*(256*1024)-offset-size;
    else if (readahead>=128*1024)
      readahead=(readahead+offset+size)/(64*1024)*(64*1024)-offset-size;
  }
  if (offset+size+readahead>initialsize)
    readahead=size_round_up_to_page(initialsize-offset-size);
  rto=of->streams[streamid].requestedto;
  if (of->streams[streamid].lastuse<ctime-30)
    rto=0;
  if (rto<offset+size+readahead)
    of->streams[streamid].requestedto=offset+size+readahead;
//  debug(D_NOTICE, "rto=%lu", rto);
  if (rto>offset+size){
    if (rto>offset+size+readahead)
      return;
    first_page_id=rto/PSYNC_FS_PAGE_SIZE;
    pagecnt=(offset+size+readahead-rto)/PSYNC_FS_PAGE_SIZE;
  }
  else{
    first_page_id=(offset+size)/PSYNC_FS_PAGE_SIZE;
    pagecnt=readahead/PSYNC_FS_PAGE_SIZE;
  }
  pages_in_db=has_pages_in_db(hash, first_page_id, pagecnt, 1);
  for (i=0; i<pagecnt; i++){
    if (pages_in_db[i])
      continue;
    if (has_page_in_cache_by_hash(hash, first_page_id+i))
      continue;
    h=waiterhash_by_hash_and_pageid(hash, first_page_id+i);
    found=0;
    psync_list_for_each_element(pw, &wait_page_hash[h], psync_page_wait_t, list)
      if (pw->hash==hash && pw->pageid==first_page_id+i)        
        found=1;
    if (found)
      continue; 
//    debug(D_NOTICE, "read-aheading page %lu", first_page_id+i);
    pw=psync_new(psync_page_wait_t);
    psync_list_add_tail(&wait_page_hash[h], &pw->list);
    psync_list_init(&pw->waiters);
    pw->hash=hash;
    pw->pageid=first_page_id+i;
    pw->fileid=fileid;
    if (range && range->offset+range->length==(first_page_id+i)*PSYNC_FS_PAGE_SIZE)
      range->length+=PSYNC_FS_PAGE_SIZE;
    else{
      range=psync_new(psync_request_range_t);
      psync_list_add_tail(ranges, &range->list);
      range->offset=(first_page_id+i)*PSYNC_FS_PAGE_SIZE;
      range->length=PSYNC_FS_PAGE_SIZE;
    }
  }
  psync_free(pages_in_db);
  if (!psync_list_isempty(ranges))
    debug(D_NOTICE, "readahead=%lu, rto=%lu, offset=%lu, size=%lu, currentspeed=%u", 
          (long unsigned)readahead, (unsigned long)rto, (unsigned long)offset, (unsigned long)size, (unsigned)of->currentspeed);
}

static void psync_free_page_waiter(psync_page_waiter_t *pwt){
  pthread_cond_destroy(&pwt->cond);
  psync_free(pwt);
}

int psync_pagecache_read_unmodified_locked(psync_openfile_t *of, char *buf, uint64_t size, uint64_t offset){
  uint64_t poffset, psize, first_page_id, initialsize, hash;
  psync_uint_t pageoff, pagecnt, i, copysize, copyoff;
  psync_file_t fileid;
  psync_int_t rb, h;
  char *pbuff;
  psync_page_waiter_t *pwt;
  psync_page_wait_t *pw;
  psync_request_t *rq;
  psync_request_range_t *range;
  psync_list waiting;
  int ret;
  initialsize=of->initialsize;
  hash=of->hash;
  fileid=of->remotefileid;
  pthread_mutex_unlock(&of->mutex);
  if (offset>=initialsize)
    return 0;
  if (offset+size>initialsize)
    size=initialsize-offset;
  poffset=offset_round_down_to_page(offset);
  pageoff=offset-poffset;
  psize=size_round_up_to_page(size+pageoff);
  pagecnt=psize/PSYNC_FS_PAGE_SIZE;
  first_page_id=poffset/PSYNC_FS_PAGE_SIZE;
  psync_list_init(&waiting);
  rq=psync_new(psync_request_t);
  psync_list_init(&rq->ranges);
  range=NULL;
  lock_wait(hash);
  for (i=0; i<pagecnt; i++){
    if (i==0){
      copyoff=pageoff;
      if (size>PSYNC_FS_PAGE_SIZE-copyoff)
        copysize=PSYNC_FS_PAGE_SIZE-copyoff;
      else
        copysize=size;
      pbuff=buf;
    }
    else if (i==pagecnt-1){
      copyoff=0;
      copysize=(size+pageoff)&(PSYNC_FS_PAGE_SIZE-1);
      if (!copysize)
        copysize=PSYNC_FS_PAGE_SIZE;
      pbuff=buf+i*PSYNC_FS_PAGE_SIZE-pageoff;
    }
    else{
      copyoff=0;
      copysize=PSYNC_FS_PAGE_SIZE;
      pbuff=buf+i*PSYNC_FS_PAGE_SIZE-pageoff;
    }
    rb=check_page_in_memory_by_hash(hash, first_page_id+i, pbuff, copysize, copyoff);
    if (rb==-1)
      rb=check_page_in_database_by_hash(hash, first_page_id+i, pbuff, copysize, copyoff);
    if (rb!=-1){
      if (rb==copysize)
        continue;
      else{
        if (i)
          size=i*PSYNC_FS_PAGE_SIZE+rb-pageoff;
        else
          size=rb;
        break;
      }
    }
    pwt=psync_new(psync_page_waiter_t);
    pthread_cond_init(&pwt->cond, NULL);
    pwt->buff=pbuff;
    pwt->pageidx=i;
    pwt->off=copyoff;
    pwt->size=copysize;
    pwt->error=0;
    pwt->ready=0;
    psync_list_add_tail(&waiting, &pwt->listwaiter);
    h=waiterhash_by_hash_and_pageid(hash, first_page_id+i);
    psync_list_for_each_element(pw, &wait_page_hash[h], psync_page_wait_t, list)
      if (pw->hash==hash && pw->pageid==first_page_id+i)        
        goto found;
    debug(D_NOTICE, "page %lu not found", (unsigned long)(first_page_id+i));
    pw=psync_new(psync_page_wait_t);
    psync_list_add_tail(&wait_page_hash[h], &pw->list);
    psync_list_init(&pw->waiters);
    pw->hash=hash;
    pw->pageid=first_page_id+i;
    pw->fileid=fileid;
    if (range && range->offset+range->length==(first_page_id+i)*PSYNC_FS_PAGE_SIZE)
      range->length+=PSYNC_FS_PAGE_SIZE;
    else{
      range=psync_new(psync_request_range_t);
      psync_list_add_tail(&rq->ranges, &range->list);
      range->offset=(first_page_id+i)*PSYNC_FS_PAGE_SIZE;
      range->length=PSYNC_FS_PAGE_SIZE;
    }
found:
    psync_list_add_tail(&pw->waiters, &pwt->listpage);
    pwt->waiting_for=pw;
  }
  psync_pagecache_read_unmodified_readahead(of, poffset, psize, &rq->ranges, range, fileid, hash, initialsize);
  if (!psync_list_isempty(&rq->ranges)){
    unlock_wait(hash);
    rq->of=of;
    rq->fileid=fileid;
    rq->hash=hash;
    psync_fs_inc_of_refcnt_and_readers(of);
    psync_run_thread1("read unmodified", psync_pagecache_read_unmodified_thread, rq);
    if (psync_list_isempty(&waiting))
      return size;
    lock_wait(hash);
  }
  else
    psync_free(rq);
  ret=size;
  if (!psync_list_isempty(&waiting)){
    psync_list_for_each_element(pwt, &waiting, psync_page_waiter_t, listwaiter){
      while (!pwt->ready){
        debug(D_NOTICE, "waiting for page #%lu to be read", (unsigned long)pwt->waiting_for->pageid);
        pthread_cond_wait(&pwt->cond, &wait_page_mutexes[waiter_mutex_by_hash(hash)]);
        debug(D_NOTICE, "waited for page"); // not safe to use pwt->waiting_for here
      }
      if (pwt->error)
        ret=pwt->error;
      else if (pwt->rsize<pwt->size && ret>=0){
        if (pwt->rsize){
          if (pwt->pageidx)
            ret=pwt->pageidx*PSYNC_FS_PAGE_SIZE+pwt->rsize-pageoff;
          else
            ret=pwt->rsize;
        }
        else{
          if (pwt->pageidx){
            if (pwt->pageidx*PSYNC_FS_PAGE_SIZE+pwt->rsize-pageoff<ret)
              ret=pwt->pageidx*PSYNC_FS_PAGE_SIZE+pwt->rsize-pageoff;
          }
          else
            ret=pwt->rsize;
        }
      }
    }
    psync_list_for_each_element_call(&waiting, psync_page_waiter_t, listwaiter, psync_free_page_waiter);
  }
  unlock_wait(hash);
  return ret;
}

static void psync_pagecache_add_page_if_not_exists(psync_cache_page_t *page, uint64_t hash, uint64_t pageid){
  psync_cache_page_t *pg;
  psync_page_wait_t *pw;
  psync_uint_t h1, h2;
  int hasit;
  hasit=0;
  h1=pagehash_by_hash_and_pageid(hash, pageid);
  h2=waiterhash_by_hash_and_pageid(hash, pageid);
  lock_wait(hash);
  pthread_mutex_lock(&cache_mutex);
  psync_list_for_each_element(pg, &cache_hash[h1], psync_cache_page_t, list)
    if (pg->type==PAGE_TYPE_READ && pg->hash==hash && pg->pageid==pageid){
      hasit=1;
      break;
    }
  if (!hasit)
    psync_list_for_each_element(pw, &wait_page_hash[h2], psync_page_wait_t, list)
      if (pw->hash==hash && pw->pageid==pageid){
        hasit=1;
        break;
      }
  if (!hasit && has_page_in_db(hash, pageid))
    hasit=1;
  if (hasit)
    psync_pagecache_return_free_page_locked(page);
  else{
    psync_list_add_tail(&cache_hash[h1], &page->list);
    cache_pages_in_hash++;
  }
  pthread_mutex_unlock(&cache_mutex);
  unlock_wait(hash);
}

static void psync_pagecache_new_upload_to_cache(uint64_t taskid, uint64_t hash){
  char *filename;
  psync_cache_page_t *page;
  uint64_t pageid;
  ssize_t rd;
  time_t tm;
  psync_file_t fd;
  char fileidhex[sizeof(psync_fsfileid_t)*2+2];
  psync_binhex(fileidhex, &taskid, sizeof(psync_fsfileid_t));
  fileidhex[sizeof(psync_fsfileid_t)]='d';
  fileidhex[sizeof(psync_fsfileid_t)+1]=0;
  tm=psync_timer_time();
  filename=psync_strcat(psync_setting_get_string(_PS(fscachepath)), PSYNC_DIRECTORY_SEPARATOR, fileidhex, NULL);
  debug(D_NOTICE, "adding file %s to cache for hash %lu (%ld)", filename, (unsigned long)hash, (long)hash);
  fd=psync_file_open(filename, P_O_RDONLY, 0);
  if (fd==INVALID_HANDLE_VALUE){
    debug(D_ERROR, "could not open cache file %s for taskid %lu, skipping", filename, (unsigned long)taskid);
    psync_file_delete(filename);
    psync_free(filename);
    return;
  }
  pageid=0;
  while (1){
    page=psync_pagecache_get_free_page();
    rd=psync_file_read(fd, page->page, PSYNC_FS_PAGE_SIZE);
    if (rd<=0){
      psync_pagecache_return_free_page(page);
      break;
    }
    page->hash=hash;
    page->pageid=pageid;
    page->lastuse=tm;
    page->size=rd;
    page->usecnt=1;
    page->type=PAGE_TYPE_READ;
    psync_pagecache_add_page_if_not_exists(page, hash, pageid);
    if (rd<PSYNC_FS_PAGE_SIZE)
      break;
    pageid++;
  }
  psync_file_close(fd);
  psync_file_delete(filename);
  psync_free(filename);
}

static void psync_pagecache_modify_to_cache(uint64_t taskid, uint64_t hash, uint64_t oldhash){
  psync_sql_res *res;
  char *filename, *indexname;
  const char *cachepath;
  psync_cache_page_t *page;
  psync_interval_tree_t *tree, *interval;
  uint64_t pageid, off, roff, rdoff, rdlen;
  int64_t fs;
  ssize_t rd;
  psync_int_t pdb;
  time_t tm;
  psync_file_t fd;
  char fileidhex[sizeof(psync_fsfileid_t)*2+2];
  int tstarted, ret;
  tstarted=0;
  res=NULL;
  psync_binhex(fileidhex, &taskid, sizeof(psync_fsfileid_t));
  fileidhex[sizeof(psync_fsfileid_t)]='d';
  fileidhex[sizeof(psync_fsfileid_t)+1]=0;
  tm=psync_timer_time();
  cachepath=psync_setting_get_string(_PS(fscachepath));
  filename=psync_strcat(cachepath, PSYNC_DIRECTORY_SEPARATOR, fileidhex, NULL);
  fileidhex[sizeof(psync_fsfileid_t)]='i';
  indexname=psync_strcat(cachepath, PSYNC_DIRECTORY_SEPARATOR, fileidhex, NULL);
  debug(D_NOTICE, "adding blocks of file %s to cache for hash %lu (%ld), old hash %lu (%ld)", 
        filename, (unsigned long)hash, (long)hash, (unsigned long)oldhash, (long)oldhash);
  fd=psync_file_open(indexname, P_O_RDONLY, 0);
  if (unlikely(fd==INVALID_HANDLE_VALUE)){
    debug(D_ERROR, "could not open index of cache file %s for taskid %lu, skipping", indexname, (unsigned long)taskid);
    psync_file_delete(filename);
    psync_file_delete(indexname);
    psync_free(filename);
    psync_free(indexname);
    return;
  }
  tree=NULL;
  if (unlikely_log((fs=psync_file_size(fd))==-1 || psync_fs_load_interval_tree(fd, fs, &tree)==-1))
    goto err2;
  psync_file_close(fd);
  fd=psync_file_open(filename, P_O_RDONLY, 0);
  if (unlikely(fd==INVALID_HANDLE_VALUE)){
    debug(D_ERROR, "could not open cache file %s for taskid %lu, skipping", filename, (unsigned long)taskid);
    goto err1;
  }
  fs=psync_file_size(fd);
  if (unlikely_log(fs==-1))
    goto err2;
  interval=psync_interval_tree_get_first(tree);
  for (off=0; off<fs; off+=PSYNC_FS_PAGE_SIZE){
    pageid=off/PSYNC_FS_PAGE_SIZE;
    while (interval && interval->to<=off)
      interval=psync_interval_tree_get_next(interval);
    if (!interval || interval->from>=off+PSYNC_FS_PAGE_SIZE){ // full old page
      if (!tstarted){
        res=psync_sql_prep_statement("UPDATE OR IGNORE pagecache SET hash=? WHERE hash=? AND pageid=? AND type=?");
        psync_sql_start_transaction();
        tstarted=1;
      }
      else if (tstarted++>=64){
        psync_sql_free_result(res);
        psync_sql_commit_transaction();
        psync_milisleep(10);
        res=psync_sql_prep_statement("UPDATE OR IGNORE pagecache SET hash=? WHERE hash=? AND pageid=? AND type=?");
        psync_sql_start_transaction();
        tstarted=1;
      }
      psync_sql_bind_uint(res, 1, hash);
      psync_sql_bind_uint(res, 2, oldhash);
      psync_sql_bind_uint(res, 3, pageid);
      psync_sql_bind_uint(res, 4, PAGE_TYPE_READ);
      psync_sql_run(res);
    }
    else{
      if (tstarted){
        psync_sql_free_result(res);
        psync_sql_commit_transaction();
        tstarted=0;
      }
      if (interval->from<=off && interval->to>=off+PSYNC_FS_PAGE_SIZE){ // full new page
        page=psync_pagecache_get_free_page();
        rd=psync_file_pread(fd, page->page, PSYNC_FS_PAGE_SIZE, off);
        if (rd<PSYNC_FS_PAGE_SIZE && off+rd!=fs){
          psync_pagecache_return_free_page(page);
          break;
        }
        page->hash=hash;
        page->pageid=pageid;
        page->lastuse=tm;
        page->size=rd;
        page->usecnt=1;
        page->type=PAGE_TYPE_READ;
        psync_pagecache_add_page_if_not_exists(page, hash, pageid);
      }
      else { // page with both old and new fragments
        // we covered full new page and full old page cases, so this interval either ends or starts inside current page
        assert((interval->to>off && interval->to<=off+PSYNC_FS_PAGE_SIZE) || (interval->from>=off && interval->from<off+PSYNC_FS_PAGE_SIZE));
        page=psync_pagecache_get_free_page();
        pdb=check_page_in_database_by_hash(oldhash, pageid, page->page, PSYNC_FS_PAGE_SIZE, 0);
        if (pdb==-1){
          psync_pagecache_return_free_page(page);
          continue;
        }
        ret=0;
        while (1){
          if (interval->from>off){
            roff=interval->from-off;
            rdoff=interval->from;
          }
          else{
            roff=0;
            rdoff=off;
          }
          if (interval->to<off+PSYNC_FS_PAGE_SIZE)
            rdlen=interval->to-rdoff;
          else
            rdlen=PSYNC_FS_PAGE_SIZE-roff;
          assert(roff+rdlen<=PSYNC_FS_PAGE_SIZE);
//          debug(D_NOTICE, "ifrom=%lu ito=%lu roff=%lu roff=%lu rdlen=%lu", interval->from, interval->to, rdoff, roff, rdlen);
          rd=psync_file_pread(fd, page->page+roff, rdlen, rdoff);
          if (rd!=rdlen){
            ret=-1;
            break;
          }
          if (roff+rdlen>pdb)
            pdb=roff+rdlen;
          if (interval->to>off+PSYNC_FS_PAGE_SIZE)
            break;
          interval=psync_interval_tree_get_next(interval);
          if (!interval || interval->from>=off+PSYNC_FS_PAGE_SIZE)
            break;
        }
        if (unlikely_log(ret==-1)){
          psync_pagecache_return_free_page(page);
          continue;
        }
        page->hash=hash;
        page->pageid=pageid;
        page->lastuse=tm;
        page->size=pdb;
        page->usecnt=1;
        page->type=PAGE_TYPE_READ;
        psync_pagecache_add_page_if_not_exists(page, hash, pageid);
      }
    }
  }
err2:
  if (tstarted){
    psync_sql_free_result(res);
    psync_sql_commit_transaction();
  }
  psync_file_close(fd);
err1:
  psync_interval_tree_free(tree);
  psync_file_delete(filename);
  psync_free(filename);
  psync_file_delete(indexname);
  psync_free(indexname);
}

static void psync_pagecache_upload_to_cache(){
  psync_sql_res *res;
  psync_uint_row row;
  uint64_t id, type, taskid, hash, oldhash;
  while (1){
    res=psync_sql_query("SELECT id, type, taskid, hash, oldhash FROM pagecachetask ORDER BY id LIMIT 1");
    row=psync_sql_fetch_rowint(res);
    if (!row){
      upload_to_cache_thread_run=0;
      psync_sql_free_result(res);
      break;
    }
    id=row[0];
    type=row[1];
    taskid=row[2];
    hash=row[3];
    oldhash=row[4];
    psync_sql_free_result(res);
    if (type==PAGE_TASK_TYPE_CREAT)
      psync_pagecache_new_upload_to_cache(taskid, hash);
    else if (type==PAGE_TASK_TYPE_MODIFY)
      psync_pagecache_modify_to_cache(taskid, hash, oldhash);
    res=psync_sql_prep_statement("DELETE FROM pagecachetask WHERE id=?");
    psync_sql_bind_uint(res, 1, id);
    psync_sql_run_free(res);
  }
}

static void psync_pagecache_add_task(uint32_t type, uint64_t taskid, uint64_t hash, uint64_t oldhash){
  psync_sql_res *res;
  int run;
  run=0;
  res=psync_sql_prep_statement("INSERT INTO pagecachetask (type, taskid, hash, oldhash) VALUES (?, ?, ?, ?)");
  psync_sql_bind_uint(res, 1, type);
  psync_sql_bind_uint(res, 2, taskid);
  psync_sql_bind_uint(res, 3, hash);
  psync_sql_bind_uint(res, 4, oldhash);
  if (!upload_to_cache_thread_run){
    upload_to_cache_thread_run=1;
    run=1;
  }
  psync_sql_run_free(res);
  if (run)
    psync_run_thread("upload to cache", psync_pagecache_upload_to_cache);
}

void psync_pagecache_creat_to_pagecache(uint64_t taskid, uint64_t hash){
  psync_pagecache_add_task(PAGE_TASK_TYPE_CREAT, taskid, hash, 0);
}

void psync_pagecache_modify_to_pagecache(uint64_t taskid, uint64_t hash, uint64_t oldhash){
  psync_pagecache_add_task(PAGE_TASK_TYPE_MODIFY, taskid, hash, oldhash);
}

int psync_pagecache_have_all_pages_in_cache(uint64_t hash, uint64_t size){
  unsigned char *db;
  uint32_t i, pagecnt;
  pagecnt=(size+PSYNC_FS_PAGE_SIZE-1)/PSYNC_FS_PAGE_SIZE;
  db=has_pages_in_db(hash, 0, pagecnt, 0);
  for (i=0; i<pagecnt; i++)
    if (!db[i] && !has_page_in_memory_by_hash(hash, i))
      break;
  psync_free(db);
  return i==pagecnt;
}

int psync_pagecache_copy_all_pages_from_cache_to_file_locked(psync_openfile_t *of, uint64_t hash, uint64_t size){
  char buff[PSYNC_FS_PAGE_SIZE];
  uint64_t i, pagecnt;
  psync_int_t rb;
  pagecnt=(size+PSYNC_FS_PAGE_SIZE-1)/PSYNC_FS_PAGE_SIZE;
  for (i=0; i<pagecnt; i++){
    rb=check_page_in_memory_by_hash(hash, i, buff, PSYNC_FS_PAGE_SIZE, 0);
    if (rb==-1){
      rb=check_page_in_database_by_hash(hash, i, buff, PSYNC_FS_PAGE_SIZE, 0);
      if (rb==-1)
        return -1;
    }
    assertw(rb==PSYNC_FS_PAGE_SIZE || i*PSYNC_FS_PAGE_SIZE+rb==size);
    if (psync_file_pwrite(of->datafile, buff, rb, i*PSYNC_FS_PAGE_SIZE)!=rb)
      return -1;
  }
  return 0;
}

int psync_pagecache_lock_pages_in_cache(){
  if (pthread_mutex_trylock(&clean_cache_mutex))
    return -1;
  clean_cache_stoppers++;
  pthread_mutex_unlock(&clean_cache_mutex);
  return 0;
}

void psync_pagecache_unlock_pages_from_cache(){
  pthread_mutex_lock(&clean_cache_mutex);
  if (--clean_cache_stoppers && clean_cache_waiters)
    pthread_cond_broadcast(&clean_cache_cond);
  pthread_mutex_unlock(&clean_cache_mutex);
}

void psync_pagecache_resize_cache(){
  pthread_mutex_lock(&flush_cache_mutex);
  db_cache_in_pages=psync_setting_get_uint(_PS(fscachesize))/PSYNC_FS_PAGE_SIZE;
  db_cache_max_page=psync_sql_cellint("SELECT MAX(id) FROM pagecache", 0);
  if (db_cache_max_page>db_cache_in_pages){
    psync_sql_res *res;
    psync_stat_t st;
    res=psync_sql_prep_statement("DELETE FROM pagecache WHERE id>?");
    psync_sql_bind_uint(res, 1, db_cache_in_pages);
    psync_sql_run_free(res);
    db_cache_max_page=db_cache_in_pages;
    if (!psync_fstat(readcache, &st) && psync_stat_size(&st)>db_cache_in_pages*PSYNC_FS_PAGE_SIZE){
      if (likely_log(psync_file_seek(readcache, db_cache_in_pages*PSYNC_FS_PAGE_SIZE, P_SEEK_SET)!=-1)){
        assertw(psync_file_truncate(readcache)==0);
        debug(D_NOTICE, "shrunk cache to %lu pages (%lu bytes)", (unsigned long)db_cache_in_pages, (unsigned long)db_cache_in_pages*PSYNC_FS_PAGE_SIZE);
      }
    }
  }
  pthread_mutex_unlock(&flush_cache_mutex);
}

void psync_pagecache_init(){
  uint64_t i;
  char *page_data, *cache_file;
  const char *cache_dir;
  psync_sql_res *res;
  psync_cache_page_t *page;
  psync_stat_t st;
  for (i=0; i<CACHE_HASH; i++)
    psync_list_init(&cache_hash[i]);
  for (i=0; i<PAGE_WAITER_HASH; i++)
    psync_list_init(&wait_page_hash[i]);
  for (i=0; i<PAGE_WAITER_MUTEXES; i++)
    pthread_mutex_init(&wait_page_mutexes[i], NULL);
  psync_list_init(&free_pages);
  memset(cachepages_to_update, 0, sizeof(cachepages_to_update));
  pages_base=(char *)psync_malloc(CACHE_PAGES*(PSYNC_FS_PAGE_SIZE+sizeof(psync_cache_page_t)));
  page_data=pages_base;
  page=(psync_cache_page_t *)(page_data+CACHE_PAGES*PSYNC_FS_PAGE_SIZE);
  cache_pages_free=CACHE_PAGES;
  for (i=0; i<CACHE_PAGES; i++){
    page->page=page_data;
    psync_list_add_tail(&free_pages, &page->list);
    page_data+=PSYNC_FS_PAGE_SIZE;
    page++;
  }
  cache_dir=psync_setting_get_string(_PS(fscachepath));
  if (psync_stat(cache_dir, &st))
    psync_mkdir(cache_dir);
  cache_file=psync_strcat(cache_dir, PSYNC_DIRECTORY_SEPARATOR, PSYNC_DEFAULT_READ_CACHE_FILE, NULL);
  if (psync_stat(cache_file, &st))
    psync_sql_statement("DELETE FROM pagecache");
  else{
    res=psync_sql_prep_statement("DELETE FROM pagecache WHERE id>? AND type!="NTO_STR(PAGE_TYPE_FREE));
    psync_sql_bind_uint(res, 1, psync_stat_size(&st)/PSYNC_FS_PAGE_SIZE);
    psync_sql_run_free(res);
  }
  db_cache_in_pages=psync_setting_get_uint(_PS(fscachesize))/PSYNC_FS_PAGE_SIZE;
  db_cache_max_page=psync_sql_cellint("SELECT MAX(id) FROM pagecache", 0);
  if (db_cache_max_page<db_cache_in_pages){
    i=0;
    psync_sql_start_transaction();
    res=psync_sql_prep_statement("INSERT INTO pagecache (type) VALUES ("NTO_STR(PAGE_TYPE_FREE)")");
    while (db_cache_max_page+i<db_cache_in_pages && i<CACHE_PAGES*4){
      psync_sql_run(res);
      i++;
    }
    psync_sql_free_result(res);
    psync_sql_commit_transaction();
    free_db_pages+=i;
    db_cache_max_page+=i;
    debug(D_NOTICE, "inserted %lu new free pages to database, db_cache_in_pages=%lu, db_cache_max_page=%lu", 
                    (unsigned long)i, (unsigned long)db_cache_in_pages, (unsigned long)db_cache_max_page);
  }
  readcache=psync_file_open(cache_file, P_O_RDWR, P_O_CREAT);
  psync_free(cache_file);
  if (db_cache_max_page>db_cache_in_pages)
    psync_pagecache_resize_cache();
  free_db_pages=psync_sql_cellint("SELECT COUNT(*) FROM pagecache WHERE type="NTO_STR(PAGE_TYPE_FREE), 0);
  pthread_mutex_lock(&flush_cache_mutex);
  check_disk_full();
  pthread_mutex_unlock(&flush_cache_mutex);
  psync_sql_lock();
  if (psync_sql_cellint("SELECT COUNT(*) FROM pagecachetask", 0)){
    psync_run_thread("upload to cache", psync_pagecache_upload_to_cache);
    upload_to_cache_thread_run=1;
  }
  psync_sql_unlock();
  psync_timer_register(psync_pagecache_flush_timer, PSYNC_FS_DISK_FLUSH_SEC, NULL);
}

void clean_cache_del(void *delcache, psync_pstat *st){
  int ret;
  if (!psync_stat_isfolder(&st->stat) && (delcache || psync_filename_cmp(st->name, PSYNC_DEFAULT_READ_CACHE_FILE))){
    ret=psync_file_delete(st->path);
    debug(D_NOTICE, "delete of %s=%d", st->path, ret);
  }
}

void psync_pagecache_clean_cache(){
  const char *cache_dir;
  cache_dir=psync_setting_get_string(_PS(fscachepath));
  if (readcache!=INVALID_HANDLE_VALUE){
    psync_file_seek(readcache, 0, P_SEEK_SET);
    psync_file_truncate(readcache);
    psync_list_dir(cache_dir, clean_cache_del, NULL);
  }
  else
    psync_list_dir(cache_dir, clean_cache_del, (void *)1);
}

