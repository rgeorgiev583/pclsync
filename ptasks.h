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

#ifndef _PSYNC_TASKS_H
#define _PSYNC_TASKS_H

#include "pcompiler.h"
#include "psynclib.h"

#define PSYNC_TASK_DOWNLOAD 0
#define PSYNC_TASK_UPLOAD   1
#define PSYNC_TASK_DWLUPL_MASK 1

#define PSYNC_TASK_FOLDER   0
#define PSYNC_TASK_FILE     2

#define PSYNC_TASK_TYPE_OFF 2

#define PSYNC_TASK_TYPE_CREATE 0

#define PSYNC_CREATE_LOCAL_FOLDER  ((PSYNC_TASK_TYPE_CREATE<<PSYNC_TASK_TYPE_OFF)+PSYNC_TASK_FOLDER+PSYNC_TASK_DOWNLOAD)
#define PSYNC_DOWNLOAD_FILE        ((PSYNC_TASK_TYPE_CREATE<<PSYNC_TASK_TYPE_OFF)+PSYNC_TASK_FILE+PSYNC_TASK_DOWNLOAD)


void psync_task_create_local_folder(const char *path, uint64_t folderid, psync_syncid_t syncid) PSYNC_NONNULL(1);
void psync_task_download_file(const char *path, uint64_t fileid, psync_syncid_t syncid) PSYNC_NONNULL(1);

#endif