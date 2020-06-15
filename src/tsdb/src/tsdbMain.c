/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "tsdbMain.h"
#include "os.h"
#include "talgo.h"
#include "taosdef.h"
#include "tchecksum.h"
#include "tscompression.h"
#include "tsdb.h"
#include "ttime.h"
#include "tulog.h"

#include <pthread.h>
#include <sys/stat.h>

#define TSDB_CFG_FILE_NAME "config"
#define TSDB_DATA_DIR_NAME "data"
#define TSDB_META_FILE_NAME "meta"
#define TSDB_META_FILE_INDEX 10000000

// Function declaration
int32_t tsdbCreateRepo(char *rootDir, STsdbCfg *pCfg) {
  if (mkdir(rootDir, 0755) < 0) {
    tsdbError("vgId:%d failed to create rootDir %s since %s", pCfg->tsdbId, rootDir, strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  if (tsdbCheckAndSetDefaultCfg(pCfg) < 0) return -1;

  if (tsdbSetRepoEnv(rootDir, pCfg) < 0) return -1;

  tsdbTrace(
      "vgId%d tsdb env create succeed! cacheBlockSize %d totalBlocks %d maxTables %d daysPerFile %d keep "
      "%d minRowsPerFileBlock %d maxRowsPerFileBlock %d precision %d compression %d",
      pCfg->tsdbId, pCfg->cacheBlockSize, pCfg->totalBlocks, pCfg->maxTables, pCfg->daysPerFile, pCfg->keep,
      pCfg->minRowsPerFileBlock, pCfg->maxRowsPerFileBlock, pCfg->precision, pCfg->compression);
  return 0;
}

int32_t tsdbDropRepo(char *rootDir) {
  return tsdbUnsetRepoEnv(rootDir);
}

TSDB_REPO_T *tsdbOpenRepo(char *rootDir, STsdbAppH *pAppH) {
  STsdbCfg   config = {0};
  STsdbRepo *pRepo = NULL;

  if (tsdbLoadConfig(rootDir, &config) < 0) {
    tsdbError("failed to open repo in rootDir %s since %s", rootDir, tstrerror(terrno));
    return NULL;
  }

  pRepo = tsdbNewRepo(rootDir, pAppH, &config);
  if (pRepo == NULL) {
    tsdbError("failed to open repo in rootDir %s since %s", rootDir, tstrerror(terrno));
    return NULL;
  }

  if (tsdbOpenMeta(pRepo) < 0) {
    tsdbError("vgId:%d failed to open meta since %s", REPO_ID(pRepo), tstrerror(terrno));
    goto _err;
  }

  if (tsdbOpenBufPool(pRepo) < 0) {
    tsdbError("vgId:%d failed to open buffer pool since %s", REPO_ID(pRepo), tstrerror(terrno));
    goto _err;
  }

  if (tsdbOpenFileH(pRepo) < 0) {
    tsdbError("vgId:%d failed to open file handle since %s", REPO_ID(pRepo), tstrerror(terrno));
    goto _err;
  }

  if (tsdbRestoreInfo(pRepo) < 0) {
    tsdbError("vgId:%d failed to restore info from file since %s", REPO_ID(pRepo), tstrerror(terrno));
    goto _err;
  }

  // pRepo->state = TSDB_REPO_STATE_ACTIVE;

  tsdbTrace("vgId:%d open tsdb repository succeed!", REPO_ID(pRepo));

  return (TSDB_REPO_T *)pRepo;

_err:
  tsdbCloseRepo(pRepo, false);
  tsdbFreeRepo(pRepo);
  return NULL;
}

void tsdbCloseRepo(TSDB_REPO_T *repo, int toCommit) {
  if (repo == NULL) return;

  STsdbRepo *pRepo = (STsdbRepo *)repo;

  // TODO: wait for commit over

  tsdbCloseFileH(pRepo);
  tsdbCloseBufPool(pRepo);
  tsdbCloseMeta(pRepo);
  tsdbTrace("vgId:%d repository is closed", REPO_ID(pRepo));

  return 0;
}

int32_t tsdbInsertData(TSDB_REPO_T *repo, SSubmitMsg *pMsg, SShellSubmitRspMsg *pRsp) {
  STsdbRepo *    pRepo = (STsdbRepo *)repo;
  SSubmitMsgIter msgIter = {0};

  if (tsdbInitSubmitMsgIter(pMsg, &msgIter) < 0) {
    tsdbError("vgId:%d failed to insert data since %s", REPO_ID(pRepo), tstrerror(terrno));
    return -1;
  }

  SSubmitBlk *pBlock = NULL;
  int32_t     code = TSDB_CODE_SUCCESS;
  int32_t     affectedrows = 0;

  TSKEY now = taosGetTimestamp(pRepo->config.precision);

  while ((pBlock = tsdbGetSubmitMsgNext(&msgIter)) != NULL) {
    if (tsdbInsertDataToTable(pRepo, pBlock, now, &affectedrows) < 0) {
      pRsp->affectedRows = htonl(affectedrows);
      return -1;
    }
  }
  pRsp->affectedRows = htonl(affectedrows);
  return 0;
}

uint32_t tsdbGetFileInfo(TSDB_REPO_T *repo, char *name, uint32_t *index, uint32_t eindex, int32_t *size) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  // STsdbMeta *pMeta = pRepo->tsdbMeta;
  STsdbFileH *pFileH = pRepo->tsdbFileH;
  uint32_t    magic = 0;
  char        fname[256] = "\0";

  struct stat fState;

  tsdbTrace("vgId:%d name:%s index:%d eindex:%d", pRepo->config.tsdbId, name, *index, eindex);
  ASSERT(*index <= eindex);

  char *sdup = strdup(pRepo->rootDir);
  char *prefix = dirname(sdup);

  if (name[0] == 0) {  // get the file from index or after, but not larger than eindex
    int fid = (*index) / 3;

    if (pFileH->numOfFGroups == 0 || fid > pFileH->fGroup[pFileH->numOfFGroups - 1].fileId) {
      if (*index <= TSDB_META_FILE_INDEX && TSDB_META_FILE_INDEX <= eindex) {
        tsdbGetMetaFileName(pRepo->rootDir, fname);
        *index = TSDB_META_FILE_INDEX;
      } else {
        tfree(sdup);
        return 0;
      }
    } else {
      SFileGroup *pFGroup =
          taosbsearch(&fid, pFileH->fGroup, pFileH->numOfFGroups, sizeof(SFileGroup), compFGroupKey, TD_GE);
      if (pFGroup->fileId == fid) {
        strcpy(fname, pFGroup->files[(*index) % 3].fname);
      } else {
        if (pFGroup->fileId * 3 + 2 < eindex) {
          strcpy(fname, pFGroup->files[0].fname);
          *index = pFGroup->fileId * 3;
        } else {
          tfree(sdup);
          return 0;
        }
      }
    }
    strcpy(name, fname + strlen(prefix));
  } else {                                 // get the named file at the specified index. If not there, return 0
    if (*index == TSDB_META_FILE_INDEX) {  // get meta file
      tsdbGetMetaFileName(pRepo->rootDir, fname);
    } else {
      int         fid = (*index) / 3;
      SFileGroup *pFGroup = tsdbSearchFGroup(pFileH, fid);
      if (pFGroup == NULL) {  // not found
        tfree(sdup);
        return 0;
      }

      SFile *pFile = &pFGroup->files[(*index) % 3];
      strcpy(fname, pFile->fname);
    }
  }

  if (stat(fname, &fState) < 0) {
    tfree(sdup);
    return 0;
  }

  tfree(sdup);
  *size = fState.st_size;
  magic = *size;

  return magic;
}


void tsdbStartStream(TSDB_REPO_T *repo) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  STsdbMeta *pMeta = pRepo->tsdbMeta;

  for (int i = 0; i < pRepo->config.maxTables; i++) {
    STable *pTable = pMeta->tables[i];
    if (pTable && pTable->type == TSDB_STREAM_TABLE) {
      pTable->cqhandle = (*pRepo->appH.cqCreateFunc)(pRepo->appH.cqH, TALBE_UID(pTable), TABLE_TID(pTable), pTable->sql,
                                                     tsdbGetTableSchema(pMeta, pTable));
    }
  }
}

STsdbCfg *tsdbGetCfg(const TSDB_REPO_T *repo) {
  ASSERT(repo != NULL);
  return &((STsdbRepo *)repo)->config;
}

int32_t tsdbConfigRepo(TSDB_REPO_T *repo, STsdbCfg *pCfg) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  STsdbCfg * pRCfg = &pRepo->config;

  if (tsdbCheckAndSetDefaultCfg(pCfg) < 0) return TSDB_CODE_TDB_INVALID_CONFIG;

  ASSERT(pRCfg->tsdbId == pCfg->tsdbId);
  ASSERT(pRCfg->cacheBlockSize == pCfg->cacheBlockSize);
  ASSERT(pRCfg->daysPerFile == pCfg->daysPerFile);
  ASSERT(pRCfg->minRowsPerFileBlock == pCfg->minRowsPerFileBlock);
  ASSERT(pRCfg->maxRowsPerFileBlock == pCfg->maxRowsPerFileBlock);
  ASSERT(pRCfg->precision == pCfg->precision);

  bool configChanged = false;
  if (pRCfg->compression != pCfg->compression) {
    configChanged = true;
    tsdbAlterCompression(pRepo, pCfg->compression);
  }
  if (pRCfg->keep != pCfg->keep) {
    configChanged = true;
    tsdbAlterKeep(pRepo, pCfg->keep);
  }
  if (pRCfg->totalBlocks != pCfg->totalBlocks) {
    configChanged = true;
    tsdbAlterCacheTotalBlocks(pRepo, pCfg->totalBlocks);
  }
  if (pRCfg->maxTables != pCfg->maxTables) {
    configChanged = true;
    tsdbAlterMaxTables(pRepo, pCfg->maxTables);
  }

  if (configChanged) tsdbSaveConfig(pRepo);

  return TSDB_CODE_SUCCESS;
}

void tsdbReportStat(void *repo, int64_t *totalPoints, int64_t *totalStorage, int64_t *compStorage) {
  ASSERT(repo != NULL);
  STsdbRepo *pRepo = repo;
  *totalPoints = pRepo->stat.pointsWritten;
  *totalStorage = pRepo->stat.totalStorage;
  *compStorage = pRepo->stat.compStorage;
}

// ----------------- INTERNAL FUNCTIONS -----------------
char *tsdbGetMetaFileName(char *rootDir) {
  int   tlen = strlen(rootDir) + strlen(TSDB_META_FILE_NAME) + 2;
  char *fname = calloc(1, tlen);
  if (fname == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return NULL;
  }

  snprintf(fname, tlen, "%s/%s", rootDir, TSDB_META_FILE_NAME);
  return fname;
}

int tsdbLockRepo(STsdbRepo *pRepo) {
  int code = pthread_mutex_lock(&pRepo->mutex);
  if (code != 0) {
    tsdbError("vgId:%d failed to lock tsdb since %s", REPO_ID(pRepo), strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(code);
    return -1;
  }
  pRepo->repoLocked = true;
  return 0;
}

int tsdbUnlockRepo(STsdbRepo *pRepo) {
  pRepo->repoLocked = false;
  int code = pthread_mutex_unlock(&pRepo->mutex);
  if (code != 0) {
    tsdbError("vgId:%d failed to unlock tsdb since %s", REPO_ID(pRepo), strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(code);
    return -1;
  }
  return 0;
}

STsdbMeta *    tsdbGetMeta(TSDB_REPO_T *pRepo) { return ((STsdbRepo *)pRepo)->tsdbMeta; }
STsdbFileH *   tsdbGetFile(TSDB_REPO_T *pRepo) { return ((STsdbRepo *)pRepo)->tsdbFileH; }
STsdbRepoInfo *tsdbGetStatus(TSDB_REPO_T *pRepo) { return NULL; }

// ----------------- LOCAL FUNCTIONS -----------------
static int32_t tsdbCheckAndSetDefaultCfg(STsdbCfg *pCfg) {
  // Check precision
  if (pCfg->precision == -1) {
    pCfg->precision = TSDB_DEFAULT_PRECISION;
  } else {
    if (!IS_VALID_PRECISION(pCfg->precision)) {
      tsdbError("vgId:%d invalid precision configuration %d", pCfg->tsdbId, pCfg->precision);
      goto _err;
    }
  }

  // Check compression
  if (pCfg->compression == -1) {
    pCfg->compression = TSDB_DEFAULT_COMPRESSION;
  } else {
    if (!IS_VALID_COMPRESSION(pCfg->compression)) {
      tsdbError("vgId:%d invalid compression configuration %d", pCfg->tsdbId, pCfg->precision);
      goto _err;
    }
  }

  // Check tsdbId
  if (pCfg->tsdbId < 0) {
    tsdbError("vgId:%d invalid vgroup ID", pCfg->tsdbId);
    goto _err;
  }

  // Check maxTables
  if (pCfg->maxTables == -1) {
    pCfg->maxTables = TSDB_DEFAULT_TABLES;
  } else {
    if (pCfg->maxTables < TSDB_MIN_TABLES || pCfg->maxTables > TSDB_MAX_TABLES) {
      tsdbError("vgId:%d invalid maxTables configuration! maxTables %d TSDB_MIN_TABLES %d TSDB_MAX_TABLES %d",
                pCfg->tsdbId, pCfg->maxTables, TSDB_MIN_TABLES, TSDB_MAX_TABLES);
      goto _err;
    }
  }

  // Check daysPerFile
  if (pCfg->daysPerFile == -1) {
    pCfg->daysPerFile = TSDB_DEFAULT_DAYS_PER_FILE;
  } else {
    if (pCfg->daysPerFile < TSDB_MIN_DAYS_PER_FILE || pCfg->daysPerFile > TSDB_MAX_DAYS_PER_FILE) {
      tsdbError(
          "vgId:%d invalid daysPerFile configuration! daysPerFile %d TSDB_MIN_DAYS_PER_FILE %d TSDB_MAX_DAYS_PER_FILE "
          "%d",
          pCfg->tsdbId, pCfg->daysPerFile, TSDB_MIN_DAYS_PER_FILE, TSDB_MAX_DAYS_PER_FILE);
      goto _err;
    }
  }

  // Check minRowsPerFileBlock and maxRowsPerFileBlock
  if (pCfg->minRowsPerFileBlock == -1) {
    pCfg->minRowsPerFileBlock = TSDB_DEFAULT_MIN_ROW_FBLOCK;
  } else {
    if (pCfg->minRowsPerFileBlock < TSDB_MIN_MIN_ROW_FBLOCK || pCfg->minRowsPerFileBlock > TSDB_MAX_MIN_ROW_FBLOCK) {
      tsdbError(
          "vgId:%d invalid minRowsPerFileBlock configuration! minRowsPerFileBlock %d TSDB_MIN_MIN_ROW_FBLOCK %d "
          "TSDB_MAX_MIN_ROW_FBLOCK %d",
          pCfg->tsdbId, pCfg->minRowsPerFileBlock, TSDB_MIN_MIN_ROW_FBLOCK, TSDB_MAX_MIN_ROW_FBLOCK);
      goto _err;
    }
  }

  if (pCfg->maxRowsPerFileBlock == -1) {
    pCfg->maxRowsPerFileBlock = TSDB_DEFAULT_MAX_ROW_FBLOCK;
  } else {
    if (pCfg->maxRowsPerFileBlock < TSDB_MIN_MAX_ROW_FBLOCK || pCfg->maxRowsPerFileBlock > TSDB_MAX_MAX_ROW_FBLOCK) {
      tsdbError(
          "vgId:%d invalid maxRowsPerFileBlock configuration! maxRowsPerFileBlock %d TSDB_MIN_MAX_ROW_FBLOCK %d "
          "TSDB_MAX_MAX_ROW_FBLOCK %d",
          pCfg->tsdbId, pCfg->maxRowsPerFileBlock, TSDB_MIN_MIN_ROW_FBLOCK, TSDB_MAX_MIN_ROW_FBLOCK);
      goto _err;
    }
  }

  if (pCfg->minRowsPerFileBlock > pCfg->maxRowsPerFileBlock) {
    tsdbError("vgId:%d invalid configuration! minRowsPerFileBlock %d maxRowsPerFileBlock %d" pCfg->tsdbId,
              pCfg->minRowsPerFileBlock, pCfg->maxRowsPerFileBlock);
    goto _err;
  }

  // Check keep
  if (pCfg->keep == -1) {
    pCfg->keep = TSDB_DEFAULT_KEEP;
  } else {
    if (pCfg->keep < TSDB_MIN_KEEP || pCfg->keep > TSDB_MAX_KEEP) {
      tsdbError(
          "vgId:%d invalid keep configuration! keep %d TSDB_MIN_KEEP %d "
          "TSDB_MAX_KEEP %d",
          pCfg->tsdbId, pCfg->keep, TSDB_MIN_KEEP, TSDB_MAX_KEEP);
      goto _err;
    }
  }

  return 0;

_err:
  terrno = TSDB_CODE_TDB_INVALID_CONFIG;
  return -1;
}

static int32_t tsdbSetRepoEnv(char *rootDir, STsdbCfg *pCfg) {
  if (tsdbSaveConfig(rootDir, pCfg) < 0) {
    tsdbError("vgId:%d failed to set TSDB environment since %s", pCfg->tsdbId, tstrerror(terrno));
    return -1;
  }

  char *dirName = tsdbGetDataDirName(rootDir);
  if (dirName == NULL) return -1;

  if (mkdir(dirName, 0755) < 0) {
    tsdbError("vgId:%d failed to create directory %s since %s", pCfg->tsdbId, dirName, strerror(errno));
    errno = TAOS_SYSTEM_ERROR(errno);
    free(dirName);
    return -1;
  }

  free(dirName);

  char *fname = tsdbGetMetaFileName(rootDir);
  if (fname == NULL) return -1;
  if (tdCreateKVStore(fname) < 0) {
    tsdbError("vgId:%d failed to open KV store since %s", pCfg->tsdbId, tstrerror(terrno));
    free(fname);
    return -1;
  }

  free(fname);
  return 0;
}

static int32_t tsdbUnsetRepoEnv(char *rootDir) {
  taosRemoveDir(rootDir);
  tsdbTrace("repository %s is removed", rootDir);
  return 0;
}

static int32_t tsdbSaveConfig(char *rootDir, STsdbCfg *pCfg) {
  int   fd = -1;
  char *fname = NULL;

  fname = tsdbGetCfgFname(rootDir);
  if (fname == NULL) {
    tsdbError("vgId:%d failed to save configuration since %s", pCfg->tsdbId, tstrerror(terrno));
    goto _err;
  }

  fd = open(fname, O_WRONLY | O_CREAT, 0755);
  if (fd < 0) {
    tsdbError("vgId:%d failed to open file %s since %s", pCfg->tsdbId, fname, strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    goto _err
  }

  if (twrite(fd, (void *)pCfg, sizeof(STsdbCfg)) < sizeof(STsdbCfg)) {
    tsdbError("vgId:%d failed to write %d bytes to file %s since %s", pCfg->tsdbId, sizeof(STsdbCfg), fname,
              strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    goto _err;
  }

  if (fsync(fd) < 0) {
    tsdbError("vgId:%d failed to fsync file %s since %s", pCfg->tsdbId, fname, strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    goto _err;
  }

  free(fname);
  close(fd);
  return 0;

_err:
  tfree(fname);
  if (fd > 0) close(fd);
  return -1;
}

static int tsdbLoadConfig(char *rootDir, STsdbCfg *pCfg) {
  char *fname = NULL;
  int   fd = -1;

  fname = tsdbGetCfgFname(rootDir);
  if (fname == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  fd = open(fname, O_RDONLY);
  if (fd < 0) {
    tsdbError("failed to open file %s since %s", fname, strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    goto _err;
  }

  if (tread(fd, (void *)pCfg, sizeof(*pCfg)) < sizeof(*pCfg)) {
    tsdbError("failed to read %d bytes from file %s since %s", sizeof(*pCfg), fname, strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    goto _err;
  }

  tfree(fname);
  close(fd);

  return 0;

_err:
  tfree(fname);
  if (fd > 0) close(fd);
  return -1;
}

static char *tsdbGetCfgFname(char *rootDir) {
  int   tlen = strlen(rootDir) + strlen(TSDB_CFG_FILE_NAME) + 2;
  char *fname = calloc(1, tlen);
  if (fname == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return NULL;
  }

  snprintf(fname, tlen, "%s/%s", rootDir, TSDB_CFG_FILE_NAME);
  return fname;
}

static char *tsdbGetDataDirName(char *rootDir) {
  int   tlen = strlen(rootDir) + strlen(TSDB_DATA_DIR_NAME) + 2;
  char *fname = calloc(1, tlen);
  if (fname == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    return NULL;
  }

  snprintf(fname, tlen, "%s/%s", rootDir, TSDB_DATA_DIR_NAME);
  return fname;
}

static STsdbRepo *tsdbNewRepo(char *rootDir, STsdbAppH *pAppH, STsdbCfg *pCfg) {
  STsdbRepo *pRepo = (STsdbRepo *)calloc(1, sizeof(STsdbRepo));
  if (pRepo == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  int code = pthread_mutex_init(&pRepo->mutex, NULL);
  if (code != 0) {
    terrno = TAOS_SYSTEM_ERROR(code);
    goto _err;
  }

  pRepo->repoLocked = false;

  pRepo->rootDir = strdup(rootDir);
  if (pRepo->rootDir == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  pRepo->config = *pCfg;
  pRepo->appH = *pAppH;

  pRepo->tsdbMeta = tsdbNewMeta(pCfg);
  if (pRepo->tsdbMeta == NULL) {
    tsdbError("vgId:%d failed to create meta since %s", REPO_ID(pRepo), tstrerror(terrno));
    goto _err;
  }

  pRepo->pPool = tsdbNewBufPool(pCfg);
  if (pRepo->pPool == NULL) {
    tsdbError("vgId:%d failed to create buffer pool since %s", REPO_ID(pRepo), tstrerror(terrno));
    goto _err;
  }

  pRepo->tsdbFileH = tsdbNewFileH(pRepo);
  if (pRepo->tsdbFileH == NULL) {
    tsdbError("vgId:%d failed to create file handle since %s", REPO_ID(pRepo), tstrerror(terrno));
    goto _err;
  }

  return pRepo;

_err:
  tsdbFreeRepo(pRepo);
  return NULL;
}

static void tsdbFreeRepo(STsdbRepo *pRepo) {
  if (pRepo) {
    tsdbFreeFileH(pRepo->tsdbFileH);
    tsdbFreeBufPool(pRepo->pPool);
    tsdbFreeMeta(pRepo->tsdbMeta);
    tsdbFreeMemTable(pRepo->mem);
    tsdbFreeMemTable(pRepo->imem);
    tfree(pRepo->rootDir);
    pthread_mutex_destroy(&pRepo->mutex);
    free(pRepo);
  }
}

static int tsdbInitSubmitMsgIter(SSubmitMsg *pMsg, SSubmitMsgIter *pIter) {
  if (pMsg == NULL) {
    terrno = TSDB_CODE_TDB_SUBMIT_MSG_MSSED_UP;
    return -1;
  }

  pMsg->length = htonl(pMsg->length);
  pMsg->numOfBlocks = htonl(pMsg->numOfBlocks);
  pMsg->compressed = htonl(pMsg->compressed);

  pIter->totalLen = pMsg->length;
  pIter->len = TSDB_SUBMIT_MSG_HEAD_SIZE;
  if (pMsg->length <= TSDB_SUBMIT_MSG_HEAD_SIZE) {
    terrno = TSDB_CODE_TDB_SUBMIT_MSG_MSSED_UP;
    return -1;
  } else {
    pIter->pBlock = pMsg->blocks;
  }

  return 0;
}

static int32_t tsdbInsertDataToTable(STsdbRepo *pRepo, SSubmitBlk *pBlock, TSKEY now, int32_t *affectedrows) {
  STsdbMeta *pMeta = pRepo->tsdbMeta;
  int64_t    points = 0;

  STable *pTable == tsdbGetTableByUid(pMeta, pBlock->uid);
  if (pTable == NULL || TABLE_TID(pTable) != pBlock->tid) {
    tsdbError("vgId:%d failed to get table to insert data, uid " PRIu64 " tid %d", REPO_ID(pRepo), pBlock->uid,
              pBlock->tid);
    terrno = TSDB_CODE_TDB_INVALID_TABLE_ID;
    return -1;
  }

  if (TABLE_TYPE(pTable) == TSDB_SUPER_TABLE) {
    tsdbError("vgId:%d invalid action trying to insert a super table %s", REPO_ID(pRepo), TABLE_CHAR_NAME(pTable));
    terrno = TSDB_CODE_TDB_INVALID_ACTION;
    return -1;
  }

  // Check schema version
  int32_t tversion = pBlock->sversion;
  STSchema * pSchema = tsdbGetTableSchema(pMeta, pTable);
  ASSERT(pSchema != NULL);
  int16_t nversion = schemaVersion(pSchema);
  if (tversion > nversion) {
    tsdbTrace("vgId:%d table %s tid %d server schema version %d is older than clien version %d, try to config.",
              REPO_ID(pRepo), TABLE_CHAR_NAME(pTable), TABLE_TID(pTable), nversion, tversion);
    void *msg = (*pRepo->appH.configFunc)(REPO_ID(pRepo), TABLE_TID(pTable));
    if (msg == NULL) return -1;

    // TODO: Deal with error her
    STableCfg *pTableCfg = tsdbCreateTableCfgFromMsg(msg);
    STable *   pTableUpdate = NULL;
    if (pTable->type == TSDB_CHILD_TABLE) {
      pTableUpdate = tsdbGetTableByUid(pMeta, pTableCfg->superUid);
    } else {
      pTableUpdate = pTable;
    }

    int32_t code = tsdbUpdateTable(pMeta, pTableUpdate, pTableCfg);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
    tsdbClearTableCfg(pTableCfg);
    rpcFreeCont(msg);

    pSchema = tsdbGetTableSchemaByVersion(pMeta, pTable, tversion);
  } else if (tversion < nversion) {
    pSchema = tsdbGetTableSchemaByVersion(pMeta, pTable, tversion);
    if (pSchema == NULL) {
      tsdbError("vgId:%d table %s tid %d invalid schema version %d from client", REPO_ID(pRepo),
                TABLE_CHAR_NAME(pTable), TABLE_TID(pTable), tversion);
      terrno = TSDB_CODE_TDB_IVD_TB_SCHEMA_VERSION;
      return -1;
    }
  } 

  SSubmitBlkIter blkIter = {0};
  SDataRow       row = NULL;

  TSKEY minKey = now - tsMsPerDay[pRepo->config.precision] * pRepo->config.keep;
  TSKEY maxKey = now + tsMsPerDay[pRepo->config.precision] * pRepo->config.daysPerFile;

  tsdbInitSubmitBlkIter(pBlock, &blkIter);
  while ((row = tsdbGetSubmitBlkNext(&blkIter)) != NULL) {
    if (dataRowKey(row) < minKey || dataRowKey(row) > maxKey) {
      tsdbError("vgId:%d table %s tid %d uid %ld timestamp is out of range! now " PRId64 " maxKey " PRId64
                " minKey " PRId64,
                REPO_ID(pRepo), TABLE_CHAR_NAME(pTable), TABLE_TID(pTable), TALBE_UID(pTable), now, minKey, maxKey);
      terrno = TSDB_CODE_TDB_TIMESTAMP_OUT_OF_RANGE;
      return -1;
    }

    if (tsdbInsertRowToMem(pRepo, row, pTable) < 0) return -1;

    (*affectedrows)++;
    points++;
  }
  pRepo->stat.pointsWritten += points * schemaNCols(pSchema);
  pRepo->stat.totalStorage += points * schemaVLen(pSchema);

  return 0;
}

static SSubmitBlk *tsdbGetSubmitMsgNext(SSubmitMsgIter *pIter) {
  SSubmitBlk *pBlock = pIter->pBlock;
  if (pBlock == NULL) return NULL;

  pBlock->len = htonl(pBlock->len);
  pBlock->numOfRows = htons(pBlock->numOfRows);
  pBlock->uid = htobe64(pBlock->uid);
  pBlock->tid = htonl(pBlock->tid);

  pBlock->sversion = htonl(pBlock->sversion);
  pBlock->padding = htonl(pBlock->padding);

  pIter->len = pIter->len + sizeof(SSubmitBlk) + pBlock->len;
  if (pIter->len >= pIter->totalLen) {
    pIter->pBlock = NULL;
  } else {
    pIter->pBlock = (SSubmitBlk *)((char *)pBlock + pBlock->len + sizeof(SSubmitBlk));
  }

  return pBlock;
}

static SDataRow tsdbGetSubmitBlkNext(SSubmitBlkIter *pIter) {
  SDataRow row = pIter->row;
  if (row == NULL) return NULL;

  pIter->len += dataRowLen(row);
  if (pIter->len >= pIter->totalLen) {
    pIter->row = NULL;
  } else {
    pIter->row = (char *)row + dataRowLen(row);
  }

  return row;
}


static int tsdbRestoreInfo(STsdbRepo *pRepo) {
  STsdbMeta * pMeta = pRepo->tsdbMeta;
  STsdbFileH *pFileH = pRepo->tsdbFileH;
  SFileGroup *pFGroup = NULL;

  SFileGroupIter iter;
  SRWHelper      rhelper = {{0}};

  if (tsdbInitReadHelper(&rhelper, pRepo) < 0) goto _err;

  tsdbInitFileGroupIter(pFileH, &iter, TSDB_ORDER_DESC);
  while ((pFGroup = tsdbGetFileGroupNext(&iter)) != NULL) {
    if (tsdbSetAndOpenHelperFile(&rhelper, pFGroup) < 0) goto _err;
    for (int i = 1; i < pRepo->config.maxTables; i++) {
      STable *pTable = pMeta->tables[i];
      if (pTable == NULL) continue;
      SCompIdx *pIdx = &rhelper.pCompIdx[i];

      if (pIdx->offset > 0 && pTable->lastKey < pIdx->maxKey) pTable->lastKey = pIdx->maxKey;
    }
  }

  tsdbDestroyHelper(&rhelper);
  return 0;

_err:
  tsdbDestroyHelper(&rhelper);
  return -1;
}

static int tsdbInitSubmitBlkIter(SSubmitBlk *pBlock, SSubmitBlkIter *pIter) {
  if (pBlock->len <= 0) return -1;
  pIter->totalLen = pBlock->len;
  pIter->len = 0;
  pIter->row = (SDataRow)(pBlock->data);
  return 0;
}

static void tsdbAlterCompression(STsdbRepo *pRepo, int8_t compression) {
  int8_t oldCompRession = pRepo->config.compression;
  pRepo->config.compression = compression;
  tsdbTrace("vgId:%d tsdb compression is changed from %d to %d", oldCompRession, compression);
}

static void tsdbAlterKeep(STsdbRepo *pRepo, int32_t keep) {
  STsdbCfg *pCfg = &pRepo->config;
  int       oldKeep = pCfg->keep;

  int maxFiles = keep / pCfg->maxTables + 3;
  if (pRepo->config.keep > keep) {
    pRepo->config.keep = keep;
    pRepo->tsdbFileH->maxFGroups = maxFiles;
  } else {
    pRepo->config.keep = keep;
    pRepo->tsdbFileH->fGroup = realloc(pRepo->tsdbFileH->fGroup, sizeof(SFileGroup));
    if (pRepo->tsdbFileH->fGroup == NULL) {
      // TODO: deal with the error
    }
    pRepo->tsdbFileH->maxFGroups = maxFiles;
  }
  tsdbTrace("vgId:%d, keep is changed from %d to %d", pRepo->config.tsdbId, oldKeep, keep);
}

static void tsdbAlterMaxTables(STsdbRepo *pRepo, int32_t maxTables) {
  int oldMaxTables = pRepo->config.maxTables;
  if (oldMaxTables < pRepo->config.maxTables) {
    // TODO
  }

  STsdbMeta *pMeta = pRepo->tsdbMeta;

  pMeta->maxTables = maxTables;
  pMeta->tables = realloc(pMeta->tables, maxTables * sizeof(STable *));
  memset(&pMeta->tables[oldMaxTables], 0, sizeof(STable *) * (maxTables - oldMaxTables));
  pRepo->config.maxTables = maxTables;

  tsdbTrace("vgId:%d, tsdb maxTables is changed from %d to %d!", pRepo->config.tsdbId, oldMaxTables, maxTables);
}

#if 0

TSKEY tsdbGetTableLastKey(TSDB_REPO_T *repo, uint64_t uid) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;

  STable *pTable = tsdbGetTableByUid(pRepo->tsdbMeta, uid);
  if (pTable == NULL) return -1;

  return TSDB_GET_TABLE_LAST_KEY(pTable);
}

#endif