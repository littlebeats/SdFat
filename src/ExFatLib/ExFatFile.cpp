/**
 * Copyright (c) 2011-2025 Bill Greiman
 * This file is part of the SdFat library for SD memory cards.
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#define DBG_FILE "ExFatFile.cpp"
#include "../common/DebugMacros.h"
#include "../common/FsUtf.h"
#include "ExFatLib.h"
//------------------------------------------------------------------------------
/** test for legal character.
 *
 * \param[in] c character to be tested.
 *
 * \return true for legal character else false.
 */
inline bool lfnLegalChar(uint8_t c) {
#if USE_UTF8_LONG_NAMES
  return !lfnReservedChar(c);
#else   // USE_UTF8_LONG_NAMES
  return !(lfnReservedChar(c) || c & 0X80);
#endif  // USE_UTF8_LONG_NAMES
}
//------------------------------------------------------------------------------
bool ExFatFile::attrib(uint8_t bits) {
  if (!isFileOrSubDir() || (bits & FS_ATTRIB_USER_SETTABLE) != bits) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  // Don't allow read-only to be set if the file is open for write.
  if ((bits & FS_ATTRIB_READ_ONLY) && isWritable()) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  m_attributes = (m_attributes & ~FS_ATTRIB_USER_SETTABLE) | bits;
  // insure sync() will update dir entry
  m_flags |= FILE_FLAG_DIR_DIRTY;
  if (!sync()) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  return true;

fail:
  return false;
}
//------------------------------------------------------------------------------
uint8_t* ExFatFile::dirCache(uint8_t set, uint8_t options) {
  DirPos_t pos = m_dirPos;
  if (m_vol->dirSeek(&pos, FS_DIR_SIZE * set) != 1) {
    return nullptr;
  }
  return m_vol->dirCache(&pos, options);
}
//------------------------------------------------------------------------------
bool ExFatFile::close() {
  bool rtn = sync();
  m_attributes = FILE_ATTR_CLOSED;
  m_flags = 0;
  return rtn;
}
//------------------------------------------------------------------------------
bool ExFatFile::contiguousRange(Sector_t* bgnSector, Sector_t* endSector) {
  if (!isContiguous()) {
    return false;
  }
  if (bgnSector) {
    *bgnSector = firstSector();
  }
  if (endSector) {
    *endSector =
        firstSector() + ((m_dataLength - 1) >> m_vol->bytesPerSectorShift());
  }
  return true;
}
//------------------------------------------------------------------------------
uint32_t ExFatFile::lbdBytesPerCluster() const {
  return m_vol ? m_vol->bytesPerCluster() : 0;
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdFillExtent(
    uint64_t logicalStartBytes,
    uint64_t lengthBytes,
    Cluster_t firstCluster,
    uint32_t clusterCount,
    LbdExtent* outExtent) const {
  if (!outExtent || !m_vol || !lengthBytes || !clusterCount ||
      firstCluster < 2 ||
      (static_cast<uint64_t>(firstCluster - 2) + clusterCount) >
          m_vol->clusterCount()) {
    return false;
  }
  outExtent->logicalStartBytes = logicalStartBytes;
  outExtent->lengthBytes = lengthBytes;
  outExtent->firstCluster = firstCluster;
  outExtent->clusterCount = clusterCount;
  outExtent->firstSector = m_vol->clusterStartSector(firstCluster);
  outExtent->lastSectorInclusive = outExtent->firstSector +
      ((lengthBytes - 1) >> m_vol->bytesPerSectorShift());
  return true;
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdFindTailCluster(Cluster_t* tailCluster) const {
  if (!tailCluster || !m_vol || !m_firstCluster || !m_dataLength) {
    return false;
  }
  const uint64_t clusterCount64 =
      (m_dataLength + m_vol->bytesPerCluster() - 1) >>
          m_vol->bytesPerClusterShift();
  if (clusterCount64 == 0 || clusterCount64 > UINT32_MAX) {
    return false;
  }
  uint32_t clusterCount = static_cast<uint32_t>(clusterCount64);
  if (isContiguous()) {
    *tailCluster = m_firstCluster + clusterCount - 1;
    return true;
  }
  Cluster_t cluster = m_firstCluster;
  while (--clusterCount) {
    Cluster_t next = 0;
    if (m_vol->fatGet(cluster, &next) <= 0) {
      return false;
    }
    cluster = next;
  }
  *tailCluster = cluster;
  return true;
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdWriteFatExtent(Cluster_t firstCluster, uint32_t clusterCount) {
  if (!m_vol || !clusterCount || firstCluster < 2 ||
      (static_cast<uint64_t>(firstCluster - 2) + clusterCount) >
          m_vol->clusterCount()) {
    return false;
  }
  for (uint32_t i = 0; i + 1 < clusterCount; ++i) {
    if (!m_vol->fatPut(firstCluster + i, firstCluster + i + 1)) {
      return false;
    }
  }
  return m_vol->fatPut(firstCluster + clusterCount - 1, EXFAT_EOC);
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdBeginRawWrite(LbdRawFileInfo* info) {
  Cluster_t tailCluster = 0;
  if (!info || !isFile() || !isWritable() ||
      (m_flags & FILE_FLAG_APPEND) || !m_firstCluster ||
      (m_dataLength & (m_vol->bytesPerSector() - 1)) ||
      (m_validLength & (m_vol->bytesPerSector() - 1)) ||
      m_validLength > m_dataLength || !lbdFindTailCluster(&tailCluster)) {
    DBG_FAIL_MACRO;
    goto fail;
  }

  *info = LbdRawFileInfo{};
  info->dataLength = m_dataLength;
  info->validLength = m_validLength;
  info->writeOffset = m_validLength;
  info->firstCluster = m_firstCluster;
  info->tailCluster = tailCluster;
  info->fatChained = !isContiguous();

  if (m_validLength < m_dataLength) {
    const uint64_t clusterBytes = m_vol->bytesPerCluster();
    const uint64_t totalClusters =
        (m_dataLength + clusterBytes - 1) >> m_vol->bytesPerClusterShift();
    uint64_t runStartBytes = 0;
    Cluster_t runFirst = m_firstCluster;
    Cluster_t cluster = m_firstCluster;
    uint32_t runCount = 1;

    for (uint64_t i = 1; i < totalClusters; ++i) {
      Cluster_t next = 0;
      if (isContiguous()) {
        next = cluster + 1;
      } else if (m_vol->fatGet(cluster, &next) <= 0) {
        DBG_FAIL_MACRO;
        goto fail;
      }

      if (next == cluster + 1) {
        runCount++;
      } else {
        const uint64_t runBytes = static_cast<uint64_t>(runCount) * clusterBytes;
        if (m_validLength < runStartBytes + runBytes) {
          if (!lbdFillExtent(
              runStartBytes,
              runBytes,
              runFirst,
              runCount,
              &info->activeExtent)) {
            DBG_FAIL_MACRO;
            goto fail;
          }
          break;
        }
        runStartBytes += runBytes;
        runFirst = next;
        runCount = 1;
      }
      cluster = next;
    }

    if (!info->activeExtent.lengthBytes) {
      const uint64_t runBytes = static_cast<uint64_t>(runCount) * clusterBytes;
      if (!lbdFillExtent(
          runStartBytes,
          runBytes,
          runFirst,
          runCount,
          &info->activeExtent)) {
        DBG_FAIL_MACRO;
        goto fail;
      }
    }
  }

  if (!sync() || !m_vol->cacheSync()) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  m_vol->cacheInvalidate();
  setNoAutoExtend(true);
  return true;

fail:
  m_error |= WRITE_ERROR;
  return false;
}
//------------------------------------------------------------------------------
LbdAllocResult ExFatFile::lbdCreateFatChainedExtentFile(
    uint64_t initialExtentBytes,
    LbdExtent* outExtent) {
  if (!outExtent || !isFile() || !isWritable() || m_firstCluster ||
      m_dataLength || m_validLength || !initialExtentBytes ||
      (initialExtentBytes & (m_vol->bytesPerCluster() - 1))) {
    return LbdAllocResult::BadState;
  }
  const uint64_t clusterCount64 =
      initialExtentBytes >> m_vol->bytesPerClusterShift();
  if (clusterCount64 == 0 || clusterCount64 > UINT32_MAX) {
    return LbdAllocResult::BadState;
  }
  const uint32_t clusterCount = static_cast<uint32_t>(clusterCount64);
  const Cluster_t firstCluster = m_vol->bitmapFind(0, clusterCount);
  if (firstCluster == 1) {
    return LbdAllocResult::NoSpace;
  }
  if (firstCluster < 2) {
    return LbdAllocResult::IoError;
  }
  if (!lbdWriteFatExtent(firstCluster, clusterCount) ||
      !m_vol->bitmapAllocateExact(firstCluster, clusterCount)) {
    return LbdAllocResult::IoError;
  }

  m_firstCluster = firstCluster;
  m_curCluster = 0;
  m_curPosition = 0;
  m_dataLength = initialExtentBytes;
  m_validLength = 0;
  m_flags &= ~FILE_FLAG_CONTIGUOUS;
  m_flags |= FILE_FLAG_DIR_DIRTY;
  setNoAutoExtend(true);
  if (!lbdFillExtent(
      0,
      initialExtentBytes,
      firstCluster,
      clusterCount,
      outExtent) || !sync()) {
    return LbdAllocResult::IoError;
  }
  return LbdAllocResult::Ok;
}
//------------------------------------------------------------------------------
LbdAllocResult ExFatFile::lbdAppendAdjacentExtent(
    uint64_t extentBytes,
    LbdExtent* outExtent) {
  Cluster_t tailCluster = 0;
  if (!lbdFindTailCluster(&tailCluster)) {
    return LbdAllocResult::BadState;
  }
  return lbdAppendAdjacentExtentAfterTail(tailCluster, extentBytes, outExtent);
}
//------------------------------------------------------------------------------
LbdAllocResult ExFatFile::lbdAppendAdjacentExtentAfterTail(
    Cluster_t tailCluster,
    uint64_t extentBytes,
    LbdExtent* outExtent) {
  if (!outExtent || !isFile() || !isWritable() || isContiguous() || !m_vol ||
      !m_firstCluster || tailCluster < 2 ||
      static_cast<uint64_t>(tailCluster - 2) >= m_vol->clusterCount() ||
      !extentBytes || (extentBytes & (m_vol->bytesPerCluster() - 1)) ||
      (m_dataLength & (m_vol->bytesPerCluster() - 1))) {
    return LbdAllocResult::BadState;
  }

  Cluster_t nextCluster = 0;
  const int8_t tailStatus = m_vol->fatGet(tailCluster, &nextCluster);
  if (tailStatus < 0) {
    return LbdAllocResult::IoError;
  }
  if (tailStatus != 0) {
    return LbdAllocResult::BadState;
  }

  const uint64_t clusterCount64 = extentBytes >> m_vol->bytesPerClusterShift();
  if (clusterCount64 == 0 || clusterCount64 > UINT32_MAX) {
    return LbdAllocResult::BadState;
  }
  const uint32_t clusterCount = static_cast<uint32_t>(clusterCount64);
  const Cluster_t firstCluster = tailCluster + 1;
  if ((static_cast<uint64_t>(firstCluster - 2) + clusterCount) >
      m_vol->clusterCount()) {
    return LbdAllocResult::NoSpace;
  }
  const BitmapRangeState rangeState =
      m_vol->bitmapRangeIsFree(firstCluster, clusterCount);
  if (rangeState == BitmapRangeState::NotFree) {
    return LbdAllocResult::WouldFragment;
  }
  if (rangeState != BitmapRangeState::Free) {
    return LbdAllocResult::IoError;
  }
  if (!lbdWriteFatExtent(firstCluster, clusterCount) ||
      !m_vol->bitmapAllocateExact(firstCluster, clusterCount) ||
      !m_vol->fatPut(tailCluster, firstCluster)) {
    return LbdAllocResult::IoError;
  }

  const uint64_t logicalStart = m_dataLength;
  m_dataLength += extentBytes;
  m_flags &= ~FILE_FLAG_CONTIGUOUS;
  m_flags |= FILE_FLAG_DIR_DIRTY;
  if (!lbdFillExtent(
      logicalStart,
      extentBytes,
      firstCluster,
      clusterCount,
      outExtent) || !sync()) {
    return LbdAllocResult::IoError;
  }
  return LbdAllocResult::Ok;
}
//------------------------------------------------------------------------------
LbdAllocResult ExFatFile::lbdAppendFreeExtent(
    uint64_t extentBytes,
    Cluster_t preferredStartCluster,
    uint32_t searchWindowClusters,
    LbdExtent* outExtent) {
  Cluster_t tailCluster = 0;
  if (!lbdFindTailCluster(&tailCluster)) {
    return LbdAllocResult::BadState;
  }
  return lbdAppendFreeExtentAfterTail(
      tailCluster,
      extentBytes,
      preferredStartCluster,
      searchWindowClusters,
      outExtent);
}
//------------------------------------------------------------------------------
LbdAllocResult ExFatFile::lbdAppendFreeExtentAfterTail(
    Cluster_t tailCluster,
    uint64_t extentBytes,
    Cluster_t preferredStartCluster,
    uint32_t searchWindowClusters,
    LbdExtent* outExtent) {
  if (!outExtent || !isFile() || !isWritable() || isContiguous() || !m_vol ||
      !m_firstCluster || tailCluster < 2 ||
      static_cast<uint64_t>(tailCluster - 2) >= m_vol->clusterCount() ||
      !extentBytes || (extentBytes & (m_vol->bytesPerCluster() - 1)) ||
      (m_dataLength & (m_vol->bytesPerCluster() - 1))) {
    return LbdAllocResult::BadState;
  }

  Cluster_t nextCluster = 0;
  const int8_t tailStatus = m_vol->fatGet(tailCluster, &nextCluster);
  if (tailStatus < 0) {
    return LbdAllocResult::IoError;
  }
  if (tailStatus != 0) {
    return LbdAllocResult::BadState;
  }

  const uint64_t clusterCount64 = extentBytes >> m_vol->bytesPerClusterShift();
  if (clusterCount64 == 0 || clusterCount64 > UINT32_MAX) {
    return LbdAllocResult::BadState;
  }
  const uint32_t clusterCount = static_cast<uint32_t>(clusterCount64);
  const Cluster_t searchStart = preferredStartCluster >= 2 ? preferredStartCluster : 0;
  const Cluster_t firstCluster = m_vol->bitmapFind(searchStart, clusterCount);
  if (firstCluster == 1) {
    return LbdAllocResult::NoSpace;
  }
  if (firstCluster < 2) {
    return LbdAllocResult::IoError;
  }
  if (searchWindowClusters && searchStart >= 2) {
    const uint64_t windowEnd = static_cast<uint64_t>(searchStart) + searchWindowClusters;
    if (firstCluster < searchStart || firstCluster >= windowEnd) {
      return LbdAllocResult::NoSpace;
    }
  }
  if (!lbdWriteFatExtent(firstCluster, clusterCount) ||
      !m_vol->bitmapAllocateExact(firstCluster, clusterCount) ||
      !m_vol->fatPut(tailCluster, firstCluster)) {
    return LbdAllocResult::IoError;
  }

  const uint64_t logicalStart = m_dataLength;
  m_dataLength += extentBytes;
  m_flags &= ~FILE_FLAG_CONTIGUOUS;
  m_flags |= FILE_FLAG_DIR_DIRTY;
  if (!lbdFillExtent(
      logicalStart,
      extentBytes,
      firstCluster,
      clusterCount,
      outExtent) || !sync()) {
    return LbdAllocResult::IoError;
  }
  return LbdAllocResult::Ok;
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdCloseAfterRaw(uint64_t finalValidBytes) {
  if (!lbdCommitValidLength(finalValidBytes)) {
    return false;
  }
  if (finalValidBytes < m_dataLength && !lbdTruncateRaw(finalValidBytes)) {
    return false;
  }
  return close();
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdCommitValidLength(uint64_t validBytes) {
  if (!isFile() || !isWritable() ||
      (validBytes & (m_vol->bytesPerSector() - 1)) ||
      validBytes > m_dataLength) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  if (validBytes == 0) {
    m_curPosition = 0;
    m_curCluster = 0;
  } else if (validBytes != m_curPosition && !seekSet(validBytes)) {
    DBG_FAIL_MACRO;
    goto fail;
  } else {
    m_curPosition = validBytes;
  }
  m_validLength = validBytes;
  m_flags |= FILE_FLAG_DIR_DIRTY;
  return sync();

fail:
  m_error |= WRITE_ERROR;
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdEndRawWrite() {
  if (!isOpen()) {
    return false;
  }
  setNoAutoExtend(false);
  return true;
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdMarkRawWritten(
    uint64_t newWriteOffset,
    const LbdExtent* activeExtent) {
  if (!isFile() || !isWritable() || !activeExtent ||
      (newWriteOffset & (m_vol->bytesPerSector() - 1)) ||
      newWriteOffset > m_dataLength) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  if (newWriteOffset &&
      (newWriteOffset <= activeExtent->logicalStartBytes ||
       newWriteOffset > activeExtent->logicalStartBytes + activeExtent->lengthBytes)) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  m_curPosition = newWriteOffset;
  m_curCluster = newWriteOffset
      ? activeExtent->firstCluster +
            ((newWriteOffset - 1 - activeExtent->logicalStartBytes) >>
                m_vol->bytesPerClusterShift())
      : 0;
  return true;

fail:
  m_error |= WRITE_ERROR;
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdRawSectorForOffset(uint64_t offset, Sector_t* sector) const {
  if (!sector || !isContiguous() || !m_firstCluster ||
      (offset & (m_vol->bytesPerSector() - 1)) || offset >= m_dataLength) {
    return false;
  }
  *sector = firstSector() + (offset >> m_vol->bytesPerSectorShift());
  return true;
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdRawSectorForOffset(
    uint64_t offset,
    const LbdExtent* activeExtent,
    Sector_t* sector) const {
  if (!sector || !activeExtent || !activeExtent->lengthBytes ||
      (offset & (m_vol->bytesPerSector() - 1)) ||
      offset < activeExtent->logicalStartBytes ||
      offset >= activeExtent->logicalStartBytes + activeExtent->lengthBytes ||
      offset >= m_dataLength) {
    return false;
  }
  *sector = activeExtent->firstSector +
      ((offset - activeExtent->logicalStartBytes) >> m_vol->bytesPerSectorShift());
  return true;
}
//------------------------------------------------------------------------------
bool ExFatFile::lbdTruncateRaw(uint64_t length) {
  if (!isFile() || !isWritable() ||
      (length & (m_vol->bytesPerSector() - 1)) || length > m_dataLength) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  if (length == m_dataLength) {
    return lbdCommitValidLength(length);
  }
  if (length > m_validLength) {
    m_validLength = length;
  }
  if (!seekSet(length) || !truncate()) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  return true;

fail:
  m_error |= WRITE_ERROR;
  return false;
}
//------------------------------------------------------------------------------
void ExFatFile::fgetpos(fspos_t* pos) const {
  pos->position = m_curPosition;
  pos->cluster = m_curCluster;
}
//------------------------------------------------------------------------------
int ExFatFile::fgets(char* str, int num, const char* delim) {
  char ch;
  int n = 0;
  int r = -1;
  while ((n + 1) < num && (r = read(&ch, 1)) == 1) {
    // delete CR
    if (ch == '\r') {
      continue;
    }
    str[n++] = ch;
    if (!delim) {
      if (ch == '\n') {
        break;
      }
    } else {
      if (strchr(delim, ch)) {
        break;
      }
    }
  }
  if (r < 0) {
    // read error
    return -1;
  }
  str[n] = '\0';
  return n;
}
//------------------------------------------------------------------------------
Sector_t ExFatFile::firstSector() const {
  return m_firstCluster ? m_vol->clusterStartSector(m_firstCluster) : 0;
}
//------------------------------------------------------------------------------
void ExFatFile::fsetpos(const fspos_t* pos) {
  m_curPosition = pos->position;
  m_curCluster = pos->cluster;
}
//------------------------------------------------------------------------------
bool ExFatFile::getAccessDateTime(uint16_t* pdate, uint16_t* ptime) {
  const DirFile_t* df = reinterpret_cast<DirFile_t*>(
      m_vol->dirCache(&m_dirPos, FsCache::CACHE_FOR_READ));
  if (!df) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  *pdate = getLe16(df->accessDate);
  *ptime = getLe16(df->accessTime);
  return true;

fail:
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::getCreateDateTime(uint16_t* pdate, uint16_t* ptime) {
  const DirFile_t* df = reinterpret_cast<DirFile_t*>(
      m_vol->dirCache(&m_dirPos, FsCache::CACHE_FOR_READ));
  if (!df) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  *pdate = getLe16(df->createDate);
  *ptime = getLe16(df->createTime);
  return true;

fail:
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::getModifyDateTime(uint16_t* pdate, uint16_t* ptime) {
  const DirFile_t* df = reinterpret_cast<DirFile_t*>(
      m_vol->dirCache(&m_dirPos, FsCache::CACHE_FOR_READ));
  if (!df) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  *pdate = getLe16(df->modifyDate);
  *ptime = getLe16(df->modifyTime);
  return true;

fail:
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::isBusy() { return m_vol->isBusy(); }
//------------------------------------------------------------------------------
bool ExFatFile::open(const char* path, oflag_t oflag) {
  return open(ExFatVolume::cwv(), path, oflag);
}
//------------------------------------------------------------------------------
bool ExFatFile::open(ExFatVolume* vol, const char* path, oflag_t oflag) {
  return vol && open(vol->vwd(), path, oflag);
}
//------------------------------------------------------------------------------
bool ExFatFile::open(ExFatFile* dirFile, const char* path, oflag_t oflag) {
  ExFatFile tmpDir;
  ExName_t fname;
  // error if already open
  if (isOpen() || !dirFile->isDir()) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  if (isDirSeparator(*path)) {
    while (isDirSeparator(*path)) {
      path++;
    }
    if (*path == 0) {
      return openRoot(dirFile->m_vol);
    }
    if (!tmpDir.openRoot(dirFile->m_vol)) {
      DBG_FAIL_MACRO;
      goto fail;
    }
    dirFile = &tmpDir;
  }
  while (1) {
    if (!parsePathName(path, &fname, &path)) {
      DBG_FAIL_MACRO;
      goto fail;
    }
    if (*path == 0) {
      break;
    }
    if (!openPrivate(dirFile, &fname, O_RDONLY)) {
      DBG_WARN_MACRO;
      goto fail;
    }
    tmpDir.copy(this);
    dirFile = &tmpDir;
    close();
  }
  return openPrivate(dirFile, &fname, oflag);

fail:
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::open(uint32_t index, oflag_t oflag) {
  ExFatVolume* vol = ExFatVolume::cwv();
  return vol ? open(vol->vwd(), index, oflag) : false;
}
//------------------------------------------------------------------------------
bool ExFatFile::open(ExFatFile* dirFile, uint32_t index, oflag_t oflag) {
  if (dirFile->seekSet(FS_DIR_SIZE * index) && openNext(dirFile, oflag)) {
    if (dirIndex() == index) {
      return true;
    }
    close();
    DBG_FAIL_MACRO;
  }
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::openCwd() {
  if (isOpen() || !ExFatVolume::cwv()) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  this->copy(ExFatVolume::cwv()->vwd());
  rewind();
  return true;

fail:
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::openNext(ExFatFile* dir, oflag_t oflag) {
  if (isOpen() || !dir->isDir() || (dir->curPosition() & 0X1F)) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  return openPrivate(dir, nullptr, oflag);

fail:
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::openPrivate(ExFatFile* dir, ExName_t* fname, oflag_t oflag) {
  int n;
  uint8_t modeFlags;
  uint8_t* cache __attribute__((unused));
  DirPos_t freePos __attribute__((unused));
  DirFile_t* dirFile;
  DirStream_t* dirStream;
  DirName_t* dirName;
  uint8_t buf[FS_DIR_SIZE];
  uint8_t freeCount = 0;
  uint8_t freeNeed = 3;
  bool inSet = false;

  // error if already open, no access mode, or no directory.
  if (isOpen() || !dir->isDir()) {
    DBG_FAIL_MACRO;
    goto fail;
  }

  switch (oflag & O_ACCMODE) {
    case O_RDONLY:
      modeFlags = FILE_FLAG_READ;
      break;
    case O_WRONLY:
      modeFlags = FILE_FLAG_WRITE;
      break;
    case O_RDWR:
      modeFlags = FILE_FLAG_READ | FILE_FLAG_WRITE;
      break;
    default:
      DBG_FAIL_MACRO;
      goto fail;
  }
  modeFlags |= (oflag & O_APPEND) ? FILE_FLAG_APPEND : 0;

  if (fname) {
    freeNeed = 2 + (fname->nameLength + 14) / 15;
    dir->rewind();
  }

  while (1) {
    n = dir->read(buf, FS_DIR_SIZE);
    if (n == 0) {
      goto create;
    }
    if (n != FS_DIR_SIZE) {
      DBG_FAIL_MACRO;
      goto fail;
    }
    if (!(buf[0] & EXFAT_TYPE_USED)) {
      // Unused entry.
      if (freeCount == 0) {
        freePos.position = dir->curPosition() - FS_DIR_SIZE;
        freePos.cluster = dir->curCluster();
      }
      if (freeCount < freeNeed) {
        freeCount++;
      }
      if (buf[0] == EXFAT_TYPE_END_DIR) {
        if (fname) {
          goto create;
        }
        // Likely openNext call.
        DBG_WARN_MACRO;
        goto fail;
      }
      inSet = false;
    } else if (!inSet) {
      if (freeCount < freeNeed) {
        freeCount = 0;
      }
      if (buf[0] != EXFAT_TYPE_FILE) {
        continue;
      }
      inSet = true;
      memset(this, 0, sizeof(ExFatFile));
      dirFile = reinterpret_cast<DirFile_t*>(buf);
      m_setCount = dirFile->setCount;
      m_attributes = getLe16(dirFile->attributes) & FS_ATTRIB_COPY;
      if (!(m_attributes & FS_ATTRIB_DIRECTORY)) {
        m_attributes |= FILE_ATTR_FILE;
      }
      m_vol = dir->volume();
      m_dirPos.cluster = dir->curCluster();
      m_dirPos.position = dir->curPosition() - FS_DIR_SIZE;
      m_dirPos.isContiguous = dir->isContiguous();
    } else if (buf[0] == EXFAT_TYPE_STREAM) {
      dirStream = reinterpret_cast<DirStream_t*>(buf);
      m_flags = modeFlags;
      if (dirStream->flags & EXFAT_FLAG_CONTIGUOUS) {
        m_flags |= FILE_FLAG_CONTIGUOUS;
      }
      m_validLength = getLe64(dirStream->validLength);
      m_firstCluster = getLe32(dirStream->firstCluster);
      m_dataLength = getLe64(dirStream->dataLength);
      if (!fname) {
        goto found;
      }
      fname->reset();
      if (fname->nameLength != dirStream->nameLength ||
          fname->nameHash != getLe16(dirStream->nameHash)) {
        inSet = false;
      }
    } else if (buf[0] == EXFAT_TYPE_NAME) {
      dirName = reinterpret_cast<DirName_t*>(buf);
      if (!cmpName(dirName, fname)) {
        inSet = false;
        continue;
      }
      if (fname->atEnd()) {
        goto found;
      }
    } else {
      inSet = false;
    }
  }

found:
  // Don't open if create only.
  if (oflag & O_EXCL) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  // Write, truncate, or at end is an error for a directory or read-only file.
  if ((oflag & (O_TRUNC | O_AT_END)) || (m_flags & FILE_FLAG_WRITE)) {
    if (isSubDir() || isReadOnly() || EXFAT_READ_ONLY) {
      DBG_FAIL_MACRO;
      goto fail;
    }
  }

#if !EXFAT_READ_ONLY
  if (oflag & O_TRUNC) {
    if (!(m_flags & FILE_FLAG_WRITE)) {
      DBG_FAIL_MACRO;
      goto fail;
    }
    if (!truncate(0)) {
      DBG_FAIL_MACRO;
      goto fail;
    }
  } else if ((oflag & O_AT_END) && !seekSet(fileSize())) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  if (isWritable()) {
    m_attributes |= FS_ATTRIB_ARCHIVE;
  }
#endif  // !EXFAT_READ_ONLY
  return true;

create:
#if EXFAT_READ_ONLY
  DBG_FAIL_MACRO;
  goto fail;
#else   // EXFAT_READ_ONLY
  // don't create unless O_CREAT and write
  if (!(oflag & O_CREAT) || !(modeFlags & FILE_FLAG_WRITE) || !fname) {
    DBG_WARN_MACRO;
    goto fail;
  }
  while (freeCount < freeNeed) {
    n = dir->read(buf, FS_DIR_SIZE);
    if (n == 0) {
      Cluster_t saveCurCluster = dir->m_curCluster;
      if (!dir->addDirCluster()) {
        DBG_FAIL_MACRO;
        goto fail;
      }
      dir->m_curCluster = saveCurCluster;
      continue;
    }
    if (n != FS_DIR_SIZE) {
      DBG_FAIL_MACRO;
      goto fail;
    }
    if (freeCount == 0) {
      freePos.position = dir->curPosition() - FS_DIR_SIZE;
      freePos.cluster = dir->curCluster();
    }
    freeCount++;
  }
  freePos.isContiguous = dir->isContiguous();
  memset(this, 0, sizeof(ExFatFile));
  m_vol = dir->volume();
  m_attributes = FILE_ATTR_FILE | FS_ATTRIB_ARCHIVE;
  m_dirPos = freePos;
  fname->reset();
  for (uint8_t i = 0; i < freeNeed; i++) {
    cache = dirCache(i, FsCache::CACHE_FOR_WRITE);
    if (!cache || (cache[0] & 0x80)) {
      DBG_FAIL_MACRO;
      goto fail;
    }
    memset(cache, 0, FS_DIR_SIZE);
    if (i == 0) {
      dirFile = reinterpret_cast<DirFile_t*>(cache);
      dirFile->type = EXFAT_TYPE_FILE;
      m_setCount = freeNeed - 1;
      dirFile->setCount = m_setCount;

      if (FsDateTime::callback) {
        uint16_t date, time;
        uint8_t ms10;
        FsDateTime::callback(&date, &time, &ms10);
        setLe16(dirFile->createDate, date);
        setLe16(dirFile->createTime, time);
        dirFile->createTimeMs = ms10;
      } else {
        setLe16(dirFile->createDate, FS_DEFAULT_DATE);
        setLe16(dirFile->modifyDate, FS_DEFAULT_DATE);
        setLe16(dirFile->accessDate, FS_DEFAULT_DATE);
        if (FS_DEFAULT_TIME) {
          setLe16(dirFile->createTime, FS_DEFAULT_TIME);
          setLe16(dirFile->modifyTime, FS_DEFAULT_TIME);
          setLe16(dirFile->accessTime, FS_DEFAULT_TIME);
        }
      }
    } else if (i == 1) {
      dirStream = reinterpret_cast<DirStream_t*>(cache);
      dirStream->type = EXFAT_TYPE_STREAM;
      dirStream->flags = EXFAT_FLAG_ALWAYS1;
      m_flags = modeFlags | FILE_FLAG_DIR_DIRTY;
      dirStream->nameLength = fname->nameLength;
      setLe16(dirStream->nameHash, fname->nameHash);
    } else {
      dirName = reinterpret_cast<DirName_t*>(cache);
      dirName->type = EXFAT_TYPE_NAME;
      for (size_t k = 0; k < 15; k++) {
        if (fname->atEnd()) {
          break;
        }
        uint16_t u = fname->get16();
        setLe16(dirName->unicode + 2 * k, u);
      }
    }
  }
  return sync();
#endif  // EXFAT_READ_ONLY

fail:
  // close file
  m_attributes = FILE_ATTR_CLOSED;
  m_flags = 0;
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::openRoot(ExFatVolume* vol) {
  if (isOpen()) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  memset(this, 0, sizeof(ExFatFile));
  m_attributes = FILE_ATTR_ROOT;
  m_vol = vol;
  m_flags = FILE_FLAG_READ;
  return true;

fail:
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::parsePathName(const char* path, ExName_t* fname,
                              const char** ptr) {
  // Skip leading spaces.
  while (*path == ' ') {
    path++;
  }
  fname->begin = path;
  fname->end = path;
  while (*path && !isDirSeparator(*path)) {
    uint8_t c = *path++;
    if (!lfnLegalChar(c)) {
      DBG_FAIL_MACRO;
      goto fail;
    }
    if (c != '.' && c != ' ') {
      // Need to trim trailing dots spaces.
      fname->end = path;
    }
  }
  // Advance to next path component.
  for (; *path == ' ' || isDirSeparator(*path); path++) {
  }
  *ptr = path;
  return hashName(fname);

fail:
  return false;
}
//------------------------------------------------------------------------------
int ExFatFile::peek() {
  uint64_t saveCurPosition = m_curPosition;
  Cluster_t saveCurCluster = m_curCluster;
  int c = read();
  m_curPosition = saveCurPosition;
  m_curCluster = saveCurCluster;
  return c;
}
//------------------------------------------------------------------------------
int ExFatFile::read(void* buf, size_t count) {
  uint8_t* dst = reinterpret_cast<uint8_t*>(buf);
  int8_t fg;
  uint64_t maxRead;
  size_t toRead;
  size_t toFill;
  size_t rtn = 0;
  size_t n;
  uint8_t* cache;
  uint16_t sectorOffset;
  Sector_t sector;
  uint32_t clusterOffset;

  if (!isReadable()) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  if (isContiguous() || isFile()) {
    if (count > (m_dataLength - m_curPosition)) {
      count = m_dataLength - m_curPosition;
    }
    maxRead = m_curPosition < m_validLength ? m_validLength - m_curPosition : 0;
    toRead = count < maxRead ? count : maxRead;
    toFill = count > toRead ? count - toRead : 0;
  } else {
    toRead = count;
    toFill = 0;
  }
  while (toRead) {
    clusterOffset = m_curPosition & m_vol->clusterMask();
    sectorOffset = clusterOffset & m_vol->sectorMask();
    if (clusterOffset == 0) {
      if (m_curPosition == 0) {
        m_curCluster =
            isRoot() ? m_vol->rootDirectoryCluster() : m_firstCluster;
      } else if (isContiguous()) {
        m_curCluster++;
      } else {
        fg = m_vol->fatGet(m_curCluster, &m_curCluster);
        if (fg < 0) {
          DBG_FAIL_MACRO;
          goto fail;
        }
        if (fg == 0) {
          // EOF if directory.
          if (isDir()) {
            break;
          }
          DBG_FAIL_MACRO;
          goto fail;
        }
      }
    }
    sector = m_vol->clusterStartSector(m_curCluster) +
             (clusterOffset >> m_vol->bytesPerSectorShift());
    if (sectorOffset != 0 || toRead < m_vol->bytesPerSector() ||
        sector == m_vol->dataCacheSector()) {
      n = m_vol->bytesPerSector() - sectorOffset;
      if (n > toRead) {
        n = toRead;
      }
      // read sector to cache and copy data to caller
      cache = m_vol->dataCachePrepare(sector, FsCache::CACHE_FOR_READ);
      if (!cache) {
        DBG_FAIL_MACRO;
        goto fail;
      }
      const uint8_t* src = cache + sectorOffset;
      memcpy(dst, src, n);
#if USE_MULTI_SECTOR_IO
    } else if (toRead >= 2 * m_vol->bytesPerSector()) {
      uint32_t ns = toRead >> m_vol->bytesPerSectorShift();
      // Limit reads to current cluster.
      uint32_t maxNs = m_vol->sectorsPerCluster() -
                       (clusterOffset >> m_vol->bytesPerSectorShift());
      if (ns > maxNs) {
        ns = maxNs;
      }
      n = ns << m_vol->bytesPerSectorShift();
      if (!m_vol->cacheSafeRead(sector, dst, ns)) {
        DBG_FAIL_MACRO;
        goto fail;
      }
#endif  // USE_MULTI_SECTOR_IO
    } else {
      // read single sector
      n = m_vol->bytesPerSector();
      if (!m_vol->cacheSafeRead(sector, dst)) {
        DBG_FAIL_MACRO;
        goto fail;
      }
    }
    dst += n;
    rtn += n;
    m_curPosition += n;
    toRead -= n;
  }
  if (toFill) {
    memset(dst, 0, toFill);
    seekCur(toFill);
    rtn += toFill;
  }
  return rtn;

fail:
  m_error |= READ_ERROR;
  return -1;
}
//------------------------------------------------------------------------------
bool ExFatFile::remove(const char* path) {
  ExFatFile file;
  if (!file.open(this, path, O_WRONLY)) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  return file.remove();

fail:
  return false;
}
//------------------------------------------------------------------------------
bool ExFatFile::seekSet(uint64_t pos) {
  uint32_t nCur;
  uint32_t nNew;
  Cluster_t tmp = m_curCluster;
  // error if file not open
  if (!isOpen()) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  // Optimize O_APPEND writes.
  if (pos == m_curPosition) {
    return true;
  }
  if (pos == 0) {
    // set position to start of file
    m_curCluster = 0;
    goto done;
  }
  if (isFile()) {
    if (pos > m_dataLength) {
      DBG_FAIL_MACRO;
      goto fail;
    }
  }
  // calculate cluster index for new position
  nNew = (pos - 1) >> m_vol->bytesPerClusterShift();
  if (isContiguous()) {
    m_curCluster = m_firstCluster + nNew;
    goto done;
  }
  // calculate cluster index for current position
  nCur = (m_curPosition - 1) >> m_vol->bytesPerClusterShift();
  if (nNew < nCur || m_curPosition == 0) {
    // must follow chain from first cluster
    m_curCluster = isRoot() ? m_vol->rootDirectoryCluster() : m_firstCluster;
  } else {
    // advance from curPosition
    nNew -= nCur;
  }
  while (nNew--) {
    if (m_vol->fatGet(m_curCluster, &m_curCluster) <= 0) {
      DBG_FAIL_MACRO;
      goto fail;
    }
  }

done:
  m_curPosition = pos;
  return true;

fail:
  m_curCluster = tmp;
  return false;
}
