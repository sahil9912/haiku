/*
 * Copyright 2013, Haiku, Inc. All rights reserved.
 * Copyright 2008, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Ingo Weinhold, ingo_weinhold@gmx.de
 *		Siarzhuk Zharski, zharik@gmx.li
 */
#ifndef BASIC_TERMINAL_BUFFER_H
#define BASIC_TERMINAL_BUFFER_H

#include <limits.h>
#include <stack>

#include "HistoryBuffer.h"
#include "TermPos.h"
#include "UTF8Char.h"


class BString;
class TerminalCharClassifier;
struct TerminalLine;


struct TerminalBufferDirtyInfo {
	int32	linesScrolled;			// number of lines added to the history
	int32	dirtyTop;				// dirty line range
	int32	dirtyBottom;			//
	bool	invalidateAll;
	bool	messageSent;			// listener has been notified

	bool IsDirtyRegionValid() const
	{
		return dirtyTop <= dirtyBottom;
	}

	void ExtendDirtyRegion(int32 top, int32 bottom)
	{
		if (top < dirtyTop)
			dirtyTop = top;
		if (bottom > dirtyBottom)
			dirtyBottom = bottom;
	}

	void Reset()
	{
		linesScrolled = 0;
		dirtyTop = INT_MAX;
		dirtyBottom = INT_MIN;
		invalidateAll = false;
		messageSent = false;
	}

	TerminalBufferDirtyInfo()
	{
		Reset();
	}
};


class BasicTerminalBuffer {
public:
								BasicTerminalBuffer();
	virtual						~BasicTerminalBuffer();

			status_t			Init(int32 width, int32 height,
									int32 historyCapacity);

			int32				Width() const		{ return fWidth; }
			int32				Height() const		{ return fHeight; }
	inline	int32				HistorySize() const;
	inline	int32				HistoryCapacity() const;

			bool				IsAlternateScreenActive() const
									{ return fAlternateScreenActive; }

			TerminalBufferDirtyInfo& DirtyInfo()	{ return fDirtyInfo; }

	virtual	status_t			ResizeTo(int32 width, int32 height);
	virtual	status_t			ResizeTo(int32 width, int32 height,
									int32 historyCapacity);
			status_t			SetHistoryCapacity(int32 historyCapacity);
			void				Clear(bool resetCursor);

			void				SynchronizeWith(
									const BasicTerminalBuffer* other,
									int32 offset, int32 dirtyTop,
									int32 dirtyBottom);

			bool				IsFullWidthChar(int32 row, int32 column) const;
			int					GetChar(int32 row, int32 column,
									UTF8Char& character,
									uint32& attributes) const;
			void				GetCellAttributes(int32 row, int32 column,
									uint32& attributes, uint32& count) const;
			int32				GetString(int32 row, int32 firstColumn,
									int32 lastColumn, char* buffer,
									uint32& attributes) const;
			void				GetStringFromRegion(BString& string,
									const TermPos& start,
									const TermPos& end) const;
			bool				FindWord(const TermPos& pos,
									TerminalCharClassifier* classifier,
									bool findNonWords, TermPos& start,
									TermPos& end) const;
			int32				LineLength(int32 index) const;
			int32				GetLineColor(int32 index) const;

			bool				Find(const char* pattern, const TermPos& start,
									bool forward, bool caseSensitive,
									bool matchWord, TermPos& matchStart,
									TermPos& matchEnd) const;

	inline	uint32				GetAttributes();
	inline	void				SetAttributes(uint32 attributes);

			// snapshots and data capture for debugging
			void				MakeLinesSnapshots(time_t timeStamp,
									const char* fileName);
			void				StartStopDebugCapture();
			void				CaptureChar(char ch);

			// insert chars/lines
	inline	void				InsertChar(UTF8Char c);
			void				InsertChar(UTF8Char c, uint32 width);
	inline	void				InsertChar(const char* c);
	inline	void				InsertChar(const char* c, int32 length);
	inline	void				InsertChar(const char* c, int32 length,
									uint32 width);
			void				FillScreen(UTF8Char c, uint32 width, uint32 attr);

			void				InsertCR();
			void				InsertLF();
			void				InsertRI();
			void				InsertTab();
			void				SetInsertMode(int flag);
			void				InsertSpace(int32 num);
			void				InsertLines(int32 numLines);

			// delete chars/lines
	inline	void				EraseChars(int32 numChars);
			void				EraseCharsFrom(int32 first, int32 numChars);
			void				EraseAbove();
			void				EraseBelow();
			void				EraseAll();
			void				DeleteChars(int32 numChars);
	inline	void				DeleteColumns();
			void				DeleteColumnsFrom(int32 first);
			void				DeleteLines(int32 numLines);

			// get and set cursor position
	inline	void				SetCursor(int32 x, int32 y);
	inline	void				SetCursorX(int32 x);
	inline	void				SetCursorY(int32 y);
	inline	TermPos				Cursor() const			{ return fCursor; }
			void				SaveCursor();
			void				RestoreCursor();

			// move cursor
	inline	void				MoveCursorRight(int32 num);
	inline	void				MoveCursorLeft(int32 num);
	inline	void				MoveCursorUp(int32 num);
	inline	void				MoveCursorDown(int32 num);

			// scroll region
	inline	void				ScrollBy(int32 numLines);
			void				SetScrollRegion(int32 top, int32 bottom);
			void				SetOriginMode(bool enabled);
			void				SaveOriginMode();
			void				RestoreOriginMode();
			void				SetTabStop(int32 x);
			void				ClearTabStop(int32 x);
			void				ClearAllTabStops();

protected:
	virtual	void				NotifyListener();

	inline	int32				_LineIndex(int32 index) const;
	inline	TerminalLine*		_LineAt(int32 index) const;
	inline	TerminalLine*		_HistoryLineAt(int32 index,
									TerminalLine* lineBuffer) const;

	inline	void				_Invalidate(int32 top, int32 bottom);
	inline	void				_CursorChanged();
			void				_SetCursor(int32 x, int32 y, bool absolute);
			void				_InvalidateAll();

	static	TerminalLine**		_AllocateLines(int32 width, int32 count);
	static	void				_FreeLines(TerminalLine** lines, int32 count);
			void				_ClearLines(int32 first, int32 last);

			status_t			_ResizeHistory(int32 width,
									int32 historyCapacity);
			status_t			_ResizeSimple(int32 width, int32 height,
									int32 historyCapacity);
			status_t			_ResizeRewrap(int32 width, int32 height,
									int32 historyCapacity);
			status_t			_ResetTabStops(int32 width);

			void				_Scroll(int32 top, int32 bottom,
									int32 numLines);
			void				_SoftBreakLine();
			void				_PadLineToCursor();
	static	void				_TruncateLine(TerminalLine* line, int32 length);
			void				_InsertGap(int32 width);
			TerminalLine*		_GetPartialLineString(BString& string,
									int32 row, int32 startColumn,
									int32 endColumn) const;
			bool				_PreviousChar(TermPos& pos, UTF8Char& c) const;
			bool				_NextChar(TermPos& pos, UTF8Char& c) const;

protected:
			// screen width/height
			int32				fWidth;
			int32				fHeight;

			// scroll region top/bottom
			int32				fScrollTop;		// first line to scroll
			int32				fScrollBottom;	// last line to scroll (incl.)

			// line buffers for the history (ring buffer)
			TerminalLine**		fScreen;
			int32				fScreenOffset;	// index of screen line 0
			HistoryBuffer*		fHistory;

			uint32				fAttributes;

			// cursor position (origin: (0, 0))
			TermPos				fCursor;
			std::stack<TermPos>	fSavedCursors;
			bool				fSoftWrappedCursor;

			bool				fOverwriteMode;	// false for insert
			bool				fAlternateScreenActive;
			bool				fOriginMode;
			bool				fSavedOriginMode;
			bool*				fTabStops;

			int					fEncoding;
			int					fCaptureFile;

			// listener/dirty region management
			TerminalBufferDirtyInfo fDirtyInfo;
};


int32
BasicTerminalBuffer::HistorySize() const
{
	return fHistory != NULL ? fHistory->Size() : 0;
}


int32
BasicTerminalBuffer::HistoryCapacity() const
{
	return fHistory != NULL ? fHistory->Capacity() : 0;
}


uint32
BasicTerminalBuffer::GetAttributes()
{
	return fAttributes;
}


void
BasicTerminalBuffer::SetAttributes(uint32 attributes)
{
	fAttributes = attributes;
}


void
BasicTerminalBuffer::InsertChar(UTF8Char c)
{
	return InsertChar(c, 1);
}


void
BasicTerminalBuffer::InsertChar(const char* c)
{
	return InsertChar(UTF8Char(c), 1);
}


void
BasicTerminalBuffer::InsertChar(const char* c, int32 length)
{
	return InsertChar(UTF8Char(c, length), 1);
}


void
BasicTerminalBuffer::InsertChar(const char* c, int32 length, uint32 width)
{
	return InsertChar(UTF8Char(c, length), width);
}


void
BasicTerminalBuffer::EraseChars(int32 numChars)
{
	EraseCharsFrom(fCursor.x, numChars);
}


void
BasicTerminalBuffer::DeleteColumns()
{
	DeleteColumnsFrom(fCursor.x);
}


void
BasicTerminalBuffer::SetCursor(int32 x, int32 y)
{
	_SetCursor(x, y, false);
}


void
BasicTerminalBuffer::SetCursorX(int32 x)
{
	SetCursor(x, fCursor.y);
}


void
BasicTerminalBuffer::SetCursorY(int32 y)
{
	SetCursor(fCursor.x, y);
}


void
BasicTerminalBuffer::MoveCursorRight(int32 num)
{
	SetCursor(fCursor.x + num, fCursor.y);
}


void
BasicTerminalBuffer::MoveCursorLeft(int32 num)
{
	SetCursor(fCursor.x - num, fCursor.y);
}


void
BasicTerminalBuffer::MoveCursorUp(int32 num)
{
	SetCursor(fCursor.x, fCursor.y - num);
}


void
BasicTerminalBuffer::MoveCursorDown(int32 num)
{
	SetCursor(fCursor.x, fCursor.y + num);
}


void
BasicTerminalBuffer::ScrollBy(int32 numLines)
{
	_Scroll(fScrollTop, fScrollBottom, numLines);
}


#endif	// BASIC_TERMINAL_BUFFER_H
