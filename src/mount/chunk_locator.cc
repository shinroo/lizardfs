#include "config.h"
#include "mount/chunk_locator.h"

#include <unistd.h>

#include "common/MFSCommunication.h"
#include "common/mfsstrerr.h"
#include "common/strerr.h"
#include "devtools/request_log.h"
#include "mount/exceptions.h"
#include "mount/mastercomm.h"

void ReadChunkLocator::invalidateCache(uint32_t inode, uint32_t index) {
	std::unique_lock<std::mutex> lock(mutex_);
	if (cache_ && inode == inode_ && index == index_) {
		cache_ = nullptr;
	}
}

std::shared_ptr<const ChunkLocationInfo> ReadChunkLocator::locateChunk(uint32_t inode, uint32_t index) {
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (cache_ && inode == inode_ && index == index_) {
			return cache_;
		}
	}
	LOG_AVG_TILL_END_OF_SCOPE0("ReadChunkLocator::locateChunk");
	uint64_t chunkId;
	uint32_t version;
	uint64_t fileLength;
	ChunkLocationInfo::ChunkLocations locations;
#ifdef USE_LEGACY_READ_MESSAGES
	const uint8_t *chunkserversData;
	uint32_t chunkserversDataSize;
	uint8_t status = fs_readchunk(inode, index, &fileLength, &chunkId, &version,
			&chunkserversData, &chunkserversDataSize);
#else
	uint8_t status = fs_lizreadchunk(locations, chunkId, version, fileLength, inode, index);
#endif

	if (status != 0) {
		if (status == ERROR_ENOENT) {
			throw UnrecoverableReadException("Chunk locator: error sent by master server", status);
		} else {
			throw RecoverableReadException("Chunk locator: error sent by master server", status);
		}
	}

#ifdef USE_LEGACY_READ_MESSAGES
	if (chunkserversData != NULL) {
		uint32_t ip;
		uint16_t port;
		uint32_t entrySize = serializedSize(ip, port);
		for (const uint8_t *rptr = chunkserversData;
				rptr < chunkserversData + chunkserversDataSize;
				rptr += entrySize) {
			deserialize(rptr, entrySize, ip, port);
			locations.push_back(
					ChunkTypeWithAddress(NetworkAddress(ip, port),
						ChunkType::getStandardChunkType()));
		}
	}
#endif
	{
		std::unique_lock<std::mutex> lock(mutex_);
		inode_ = inode;
		index_ = index;
		cache_ = std::make_shared<ChunkLocationInfo>(chunkId, version, fileLength, locations);
		return cache_;
	}
}

void WriteChunkLocator::locateAndLockChunk(uint32_t inode, uint32_t index) {
	LOG_AVG_TILL_END_OF_SCOPE0("WriteChunkLocator::locateAndLockChunk");
	sassert(inode_ == 0 || (inode_ == inode && index_ == index));
	inode_ = inode;
	index_ = index;
	locationInfo_.locations.clear();
	uint32_t oldLockId = lockId_;
	uint64_t oldFileLength = locationInfo_.fileLength;

	uint8_t status = fs_lizwritechunk(inode, index, lockId_, locationInfo_.fileLength,
			locationInfo_.chunkId, locationInfo_.version, locationInfo_.locations);
	if (status != STATUS_OK) {
		if (status == ERROR_IO
				|| status == ERROR_NOCHUNKSERVERS
				|| status == ERROR_LOCKED
				|| status == ERROR_CHUNKBUSY
				|| status == ERROR_CHUNKLOST) {
			throw RecoverableWriteException("error sent by master server", status);
		} else {
			lockId_ = 0;
			throw UnrecoverableWriteException("error sent by master server", status);
		}
	}

	if (oldLockId != 0) {
		locationInfo_.fileLength = oldFileLength;
	}
}

void WriteChunkLocator::unlockChunk() {
	LOG_AVG_TILL_END_OF_SCOPE0("WriteChunkLocator::unlockChunk");
	sassert(lockId_ != 0);
	uint8_t status = fs_lizwriteend(locationInfo_.chunkId, lockId_,
			inode_, locationInfo_.fileLength);
	if (status == ERROR_IO) {
		// Communication with the master server failed
		throw RecoverableWriteException("Sending WRITE_END to the master failed", status);
	}
	// Master unlocked the chunk and returned some status
	lockId_ = 0;
	if (status != STATUS_OK) {
		throw UnrecoverableWriteException("Sending WRITE_END to the master failed", status);
	}
}