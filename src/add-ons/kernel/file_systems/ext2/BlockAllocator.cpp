/*
 * Copyright 2001-2010, Haiku Inc. All rights reserved.
 * This file may be used under the terms of the MIT License.
 *
 * Authors:
 *		Janito V. Ferreira Filho
 */


#include "BlockAllocator.h"

#include <util/AutoLock.h>

#include "BitmapBlock.h"
#include "Inode.h"


//#define TRACE_EXT2
#ifdef TRACE_EXT2
#	define TRACE(x...) dprintf("\33[34mext2:\33[0m " x)
#else
#	define TRACE(x...) ;
#endif


class AllocationBlockGroup : public TransactionListener {
public:
						AllocationBlockGroup();
						~AllocationBlockGroup();

			status_t	Initialize(Volume* volume, uint32 blockGroup,
							uint32 numBits);

			status_t	ScanFreeRanges();
			bool		IsFull() const;

			status_t	Allocate(Transaction& transaction, uint32 start,
							uint32 length);
			status_t	Free(Transaction& transaction, uint32 start,
							uint32 length);
			status_t	FreeAll(Transaction& transaction);
			status_t	Check(uint32 start, uint32 length);

			uint32		NumBits() const;
			uint32		FreeBits() const;
			uint32		Start() const;

			uint32		LargestStart() const;
			uint32		LargestLength() const;

			// TransactionListener implementation
			void		TransactionDone(bool success);
			void		RemovedFromTransaction();

private:
			void		_AddFreeRange(uint32 start, uint32 length);
			void		_LockInTransaction(Transaction& transaction);


			Volume*		fVolume;
			uint32		fBlockGroup;
			ext2_block_group* fGroupDescriptor;

			mutex		fLock;
			mutex		fTransactionLock;
			int32		fCurrentTransaction;

			uint32		fStart;
			uint32		fNumBits;
			uint32		fBitmapBlock;

			uint32		fFreeBits;
			uint32		fFirstFree;
			uint32		fLargestStart;
			uint32		fLargestLength;
			
			uint32		fPreviousFreeBits;
			uint32		fPreviousFirstFree;
			uint32		fPreviousLargestStart;
			uint32		fPreviousLargestLength;
};


AllocationBlockGroup::AllocationBlockGroup()
	:
	fVolume(NULL),
	fBlockGroup(0),
	fGroupDescriptor(NULL),
	fStart(0),
	fNumBits(0),
	fBitmapBlock(0),
	fFreeBits(0),
	fFirstFree(0),
	fLargestStart(0),
	fLargestLength(0),
	fPreviousFreeBits(0),
	fPreviousFirstFree(0),
	fPreviousLargestStart(0),
	fPreviousLargestLength(0)
{
	mutex_init(&fLock, "ext2 allocation block group");
	mutex_init(&fTransactionLock, "ext2 allocation block group transaction");
}


AllocationBlockGroup::~AllocationBlockGroup()
{
	mutex_destroy(&fLock);
	mutex_destroy(&fTransactionLock);
}


status_t
AllocationBlockGroup::Initialize(Volume* volume, uint32 blockGroup,
	uint32 numBits)
{
	fVolume = volume;
	fBlockGroup = blockGroup;
	fNumBits = numBits;

	status_t status = fVolume->GetBlockGroup(blockGroup, &fGroupDescriptor);
	if (status != B_OK)
		return status;

	fBitmapBlock = fGroupDescriptor->BlockBitmap();

	status = ScanFreeRanges();

	if (fGroupDescriptor->FreeBlocks() != fFreeBits) {
		TRACE("AllocationBlockGroup::Initialize(): Mismatch between counted "
			"free blocks (%lu) and what is set on the group descriptor "
			"(%lu)\n", fFreeBits, (uint32)fGroupDescriptor->FreeBlocks());
		return B_BAD_DATA;
	}

	fPreviousFreeBits = fFreeBits;
	fPreviousFirstFree = fFirstFree;
	fPreviousLargestStart = fLargestStart;
	fPreviousLargestLength = fLargestLength;
	
	return status;
}


status_t
AllocationBlockGroup::ScanFreeRanges()
{
	TRACE("AllocationBlockGroup::ScanFreeRanges()\n");
	BitmapBlock block(fVolume, fNumBits);

	if (!block.SetTo(fBitmapBlock))
		return B_ERROR;

	uint32 start = 0;
	uint32 end = 0;

	while (end < fNumBits) {
		block.FindNextUnmarked(start);
		end = start;

		if (start != block.NumBits()) {
			block.FindNextMarked(end);
			_AddFreeRange(start, end - start);
			start = end;
		}
	}

	return B_OK;
}


bool
AllocationBlockGroup::IsFull() const
{
	return fFreeBits == 0;
}


status_t
AllocationBlockGroup::Allocate(Transaction& transaction, uint32 start,
	uint32 length)
{
	TRACE("AllocationBlockGroup::Allocate(%ld,%ld)\n", start, length);
	uint32 end = start + length;
	if (end > fNumBits)
		return B_BAD_DATA;

	_LockInTransaction(transaction);

	BitmapBlock block(fVolume, fNumBits);

	if (!block.SetToWritable(transaction, fBitmapBlock))
		return B_ERROR;
	
	if (!block.Mark(start, length)) {
		TRACE("Failed to allocate blocks from %lu to %lu. Some were "
			"already allocated.\n", start, start + length);
		return B_ERROR;
	}

	fFreeBits -= length;
	fGroupDescriptor->SetFreeBlocks((uint16)fFreeBits);
	fVolume->WriteBlockGroup(transaction, fBlockGroup);

	if (start == fLargestStart) {
		if (fFirstFree == fLargestStart)
			fFirstFree += length;

		fLargestStart += length;
		fLargestLength -= length;
	} else if (start + length == fLargestStart + fLargestLength) {
		fLargestLength -= length;
	} else if (start < fLargestStart + fLargestLength
			&& start > fLargestStart) {
		uint32 firstLength = start - fLargestStart;
		uint32 secondLength = fLargestStart + fLargestLength
			- (start + length);

		if (firstLength >= secondLength) {
			fLargestLength = firstLength;
		} else {
			fLargestLength = secondLength;
			fLargestStart = start + length;
		}
	} else {
		// No need to revalidate the largest free range
		return B_OK;
	}

	if (fLargestLength < fNumBits / 2)
		block.FindLargestUnmarkedRange(fLargestStart, fLargestLength);

	return B_OK;
}


status_t
AllocationBlockGroup::Free(Transaction& transaction, uint32 start,
	uint32 length)
{
	TRACE("AllocationBlockGroup::Free(): start: %lu, length %lu\n", start,
		length);

	if (length == 0)
		return B_OK;

	uint32 end = start + length;

	if (end > fNumBits)
		return B_BAD_DATA;

	_LockInTransaction(transaction);

	BitmapBlock block(fVolume, fNumBits);

	if (!block.SetToWritable(transaction, fBitmapBlock))
		return B_ERROR;

	if (!block.Unmark(start, length)) {
		TRACE("Failed to free blocks from %lu to %lu. Some were "
			"already freed.\n", start, start + length);
		return B_ERROR;
	}

	TRACE("AllocationGroup::Free(): Unmarked bits in bitmap\n");

	if (fFirstFree > start)
		fFirstFree = start;

	if (start + length == fLargestStart) {
		fLargestStart = start;
		fLargestLength += length;
	} else if (start == fLargestStart + fLargestLength) {
		fLargestLength += length;
	} else if (fLargestLength <= fNumBits / 2) {
		// May have merged with some free blocks, becoming the largest
		uint32 newEnd = start + length;
		block.FindNextMarked(newEnd);

		uint32 newStart = start;
		block.FindPreviousMarked(newStart);

		if (newEnd - newStart > fLargestLength) {
			fLargestLength = newEnd - newStart;
			fLargestStart = newStart;
		}
	}

	fFreeBits += length;
	fGroupDescriptor->SetFreeBlocks((uint16)fFreeBits);
	fVolume->WriteBlockGroup(transaction, fBlockGroup);

	return B_OK;
}


status_t
AllocationBlockGroup::FreeAll(Transaction& transaction)
{
	return Free(transaction, 0, fNumBits);
}


uint32
AllocationBlockGroup::NumBits() const
{
	return fNumBits;
}


uint32
AllocationBlockGroup::FreeBits() const
{
	return fFreeBits;
}


uint32
AllocationBlockGroup::Start() const
{
	return fStart;
}


uint32
AllocationBlockGroup::LargestStart() const
{
	return fLargestStart;
}


uint32
AllocationBlockGroup::LargestLength() const
{
	return fLargestLength;
}


void
AllocationBlockGroup::_AddFreeRange(uint32 start, uint32 length)
{
	if (IsFull()) {
		fFirstFree = start;
		fLargestStart = start;
		fLargestLength = length;
	} else if (length > fLargestLength) {
		fLargestStart = start;
		fLargestLength = length;
	}

	fFreeBits += length;
}


void
AllocationBlockGroup::_LockInTransaction(Transaction& transaction)
{
	mutex_lock(&fLock);

	if (transaction.ID() != fCurrentTransaction) {
		mutex_unlock(&fLock);

		mutex_lock(&fTransactionLock);
		mutex_lock(&fLock);

		fCurrentTransaction = transaction.ID();
		transaction.AddListener(this);
	}

	mutex_unlock(&fLock);
}


void
AllocationBlockGroup::TransactionDone(bool success)
{
	if (success) {
		TRACE("AllocationBlockGroup::TransactionDone(): The transaction "
			"succeeded, discarding previous state\n");
		fPreviousFreeBits = fFreeBits;
		fPreviousFirstFree = fFirstFree;
		fPreviousLargestStart = fLargestStart;
		fPreviousLargestLength = fLargestLength;
	} else {
		TRACE("AllocationBlockGroup::TransactionDone(): The transaction "
			"failed, restoring to previous state\n");
		fFreeBits = fPreviousFreeBits;
		fFirstFree = fPreviousFirstFree;
		fLargestStart = fPreviousLargestStart;
		fLargestLength = fPreviousLargestLength;
	}
}


void
AllocationBlockGroup::RemovedFromTransaction()
{
	mutex_unlock(&fTransactionLock);
	fCurrentTransaction = -1;
}


BlockAllocator::BlockAllocator(Volume* volume)
	:
	fVolume(volume),
	fGroups(NULL),
	fBlocksPerGroup(0),
	fNumBlocks(0),
	fNumGroups(0)
{
	mutex_init(&fLock, "ext2 block allocator");
}


BlockAllocator::~BlockAllocator()
{
	mutex_destroy(&fLock);

	if (fGroups != NULL)
		delete [] fGroups;
}


status_t
BlockAllocator::Initialize()
{
	fBlocksPerGroup = fVolume->BlocksPerGroup();
	fNumGroups = fVolume->NumGroups();
	fFirstBlock = fVolume->FirstDataBlock();
	fNumBlocks = fVolume->NumBlocks();
	
	TRACE("BlockAllocator::Initialize(): blocks per group: %lu, block groups: "
		"%lu, first block: %lu, num blocks: %lu\n", fBlocksPerGroup,
		fNumGroups, fFirstBlock, fNumBlocks);

	fGroups = new(std::nothrow) AllocationBlockGroup[fNumGroups];
	if (fGroups == NULL)
		return B_NO_MEMORY;

	TRACE("BlockAllocator::Initialize(): allocated allocation block groups\n");

	mutex_lock(&fLock);
		// Released by _Initialize

	thread_id id = -1; // spawn_kernel_thread(
		// (thread_func)BlockAllocator::_Initialize, "ext2 block allocator",
		// B_LOW_PRIORITY, this);
	if (id < B_OK)
		return _Initialize(this);

	// mutex_transfer_lock(&fLock, id);

	// return resume_thread(id);
	panic("Failed to fall back to synchronous block allocator"
		"initialization.\n");
	return B_ERROR;
}


status_t
BlockAllocator::AllocateBlocks(Transaction& transaction, uint32 minimum,
	uint32 maximum, uint32& blockGroup, uint32& start, uint32& length)
{
	TRACE("BlockAllocator::AllocateBlocks()\n");
	MutexLocker lock(fLock);
	TRACE("BlockAllocator::AllocateBlocks(): Acquired lock\n");

	TRACE("BlockAllocator::AllocateBlocks(): transaction: %ld, min: %lu, "
		"max: %lu, block group: %lu, start: %lu, num groups: %lu\n",
		transaction.ID(), minimum, maximum, blockGroup, start, fNumGroups);

	uint32 bestStart = 0;
	uint32 bestLength = 0;
	uint32 bestGroup = 0;

	uint32 groupNum = blockGroup;

	AllocationBlockGroup* last = &fGroups[fNumGroups];
	AllocationBlockGroup* group = &fGroups[blockGroup];

	for (int32 iterations = 0; iterations < 2; iterations++) {
		for (; group < last; ++group, ++groupNum) {
			TRACE("BlockAllocator::AllocateBlocks(): Group %lu has largest "
			"length of %lu\n", groupNum, group->LargestLength());

			if (group->LargestLength() > bestLength) {
				if (start <= group->LargestStart()) {
					bestStart = group->LargestStart();
					bestLength = group->LargestLength();
					bestGroup = groupNum;

					TRACE("BlockAllocator::AllocateBlocks(): Found a better "
						"range: block group: %lu, %lu-%lu\n", groupNum,
						bestStart, bestStart + bestLength);

					if (bestLength >= maximum)
						break;
				}
			}

			start = 0;
		}

		if (bestLength >= maximum)
			break;

		groupNum = 0;

		group = &fGroups[0];
		last = &fGroups[blockGroup + 1];
	}

	if (bestLength < minimum) {
		TRACE("BlockAllocator::AllocateBlocks(): best range (length %lu) "
			"doesn't have minimum length of %lu\n", bestLength, minimum);
		return B_DEVICE_FULL;
	}

	if (bestLength > maximum)
		bestLength = maximum;

	TRACE("BlockAllocator::AllocateBlocks(): Selected range: block group %lu, "
		"%lu-%lu\n", bestGroup, bestStart, bestStart + bestLength);

	status_t status = fGroups[bestGroup].Allocate(transaction, bestStart,
		bestLength);
	if (status != B_OK) {
		TRACE("BlockAllocator::AllocateBlocks(): Failed to allocate %lu blocks "
			"inside block group %lu.\n", bestLength, bestGroup);
		return status;
	}

	start = bestStart;
	length = bestLength;
	blockGroup = bestGroup;

	return B_OK;
}


status_t
BlockAllocator::Allocate(Transaction& transaction, Inode* inode,
	off_t numBlocks, uint32 minimum, uint32& start, uint32& allocated)
{
	if (numBlocks <= 0)
		return B_ERROR;

	uint32 group = inode->ID() / fVolume->InodesPerGroup();
	uint32 preferred = 0;

	if (inode->Size() > 0) {
		// Try to allocate near it's last blocks
		ext2_data_stream* dataStream = &inode->Node().stream;
		uint32 numBlocks = inode->Size() / fVolume->BlockSize() + 1;
		uint32 lastBlock = 0;

		// DANGER! What happens with sparse files?
		if (numBlocks < EXT2_DIRECT_BLOCKS) {
			// Only direct blocks
			lastBlock = dataStream->direct[numBlocks];
		} else {
			numBlocks -= EXT2_DIRECT_BLOCKS - 1;
			uint32 numIndexes = fVolume->BlockSize() / 4;
				// block size / sizeof(int32)
			uint32 numIndexes2 = numIndexes * numIndexes;
			uint32 numIndexes3 = numIndexes2 * numIndexes;
			uint32 indexesInIndirect = numIndexes;
			uint32 indexesInDoubleIndirect = indexesInIndirect
				+ numIndexes2;
			// uint32 indexesInTripleIndirect = indexesInDoubleIndirect
				// + numIndexes3;

			uint32 doubleIndirectBlock = EXT2_DIRECT_BLOCKS + 1;
			uint32 indirectBlock = EXT2_DIRECT_BLOCKS;

			CachedBlock cached(fVolume);
			uint32* indirectData;

			if (numBlocks > indexesInDoubleIndirect) {
				// Triple indirect blocks
				indirectData = (uint32*)cached.SetTo(EXT2_DIRECT_BLOCKS + 2);
				if (indirectData == NULL)
					return B_IO_ERROR;

				uint32 index = (numBlocks - indexesInDoubleIndirect)
					/ numIndexes3;
				doubleIndirectBlock = indirectData[index];
			}

			if (numBlocks > indexesInIndirect) {
				// Double indirect blocks
				indirectData = (uint32*)cached.SetTo(doubleIndirectBlock);
				if (indirectData == NULL)
					return B_IO_ERROR;

				uint32 index = (numBlocks - indexesInIndirect) / numIndexes2;
				indirectBlock = indirectData[index];
			}

			indirectData = (uint32*)cached.SetTo(indirectBlock);
				if (indirectData == NULL)
					return B_IO_ERROR;

			uint32 index = numBlocks / numIndexes;
			lastBlock = indirectData[index];
		}

		group = (lastBlock - fFirstBlock) / fBlocksPerGroup;
		preferred = (lastBlock - fFirstBlock) % fBlocksPerGroup + 1;
	}

	// TODO: Apply some more policies

	return AllocateBlocks(transaction, minimum, minimum + 8, group, start,
		allocated);
}


status_t
BlockAllocator::Free(Transaction& transaction, uint32 start, uint32 length)
{
	TRACE("BlockAllocator::Free(%lu, %lu)\n", start, length);
	MutexLocker lock(fLock);

	if (start <= fFirstBlock) {
		panic("Trying to free superblock!\n");
		return B_BAD_DATA;
	}

	if (length == 0)
		return B_OK;

	TRACE("BlockAllocator::Free(): first block: %lu, blocks per group: %lu\n", 
		fFirstBlock, fBlocksPerGroup);

	start -= fFirstBlock;
	uint32 end = start + length - 1;

	uint32 group = start / fBlocksPerGroup;
	uint32 lastGroup = end / fBlocksPerGroup;
	start = start % fBlocksPerGroup;

	if (group == lastGroup)
		return fGroups[group].Free(transaction, start, length);

	TRACE("BlockAllocator::Free(): Freeing from group %lu: %lu, %lu\n", group,
		start, fGroups[group].NumBits() - start);

	status_t status = fGroups[group].Free(transaction, start,
		fGroups[group].NumBits() - start);
	if (status != B_OK)
		return status;

	for (++group; group < lastGroup; ++group) {
		TRACE("BlockAllocator::Free(): Freeing all from group %lu\n", group);
		status = fGroups[group].FreeAll(transaction);
		if (status != B_OK)
			return status;
	}

	TRACE("BlockAllocator::Free(): Freeing from group %lu: 0-%lu \n", group,
		end % fBlocksPerGroup);
	return fGroups[group].Free(transaction, 0, (end + 1) % fBlocksPerGroup);
}


/*static*/ status_t
BlockAllocator::_Initialize(BlockAllocator* allocator)
{
	TRACE("BlockAllocator::_Initialize()\n");
	// fLock is already heald
	Volume* volume = allocator->fVolume;

	AllocationBlockGroup* groups = allocator->fGroups;
	uint32 numGroups = allocator->fNumGroups - 1;

	uint32 freeBlocks = 0;
	TRACE("BlockAllocator::_Initialize(): free blocks: %lu\n", freeBlocks);

	for (uint32 i = 0; i < numGroups; ++i) {
		status_t status = groups[i].Initialize(volume, i, 
			allocator->fBlocksPerGroup);
		if (status != B_OK) {
			mutex_unlock(&allocator->fLock);
			return status;
		}

		freeBlocks += groups[i].FreeBits();
		TRACE("BlockAllocator::_Initialize(): free blocks: %lu\n", freeBlocks);
	}
	
	// Last block group may have less blocks
	status_t status = groups[numGroups].Initialize(volume, numGroups,
		allocator->fNumBlocks - allocator->fBlocksPerGroup * numGroups
		- allocator->fFirstBlock);
	if (status != B_OK) {
		mutex_unlock(&allocator->fLock);
		return status;
	}
	
	freeBlocks += groups[numGroups].FreeBits();

	TRACE("BlockAllocator::_Initialize(): free blocks: %lu\n", freeBlocks);

	mutex_unlock(&allocator->fLock);

	if (freeBlocks != volume->NumFreeBlocks()) {
		TRACE("Counted free blocks (%lu) doesn't match value in the "
			"superblock (%lu).\n", freeBlocks, (uint32)volume->NumFreeBlocks());
		return B_BAD_DATA;
	}

	return B_OK;
}