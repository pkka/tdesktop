/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2016 John Preston, https://desktop.telegram.org
*/
#include "stdafx.h"
#include "media/media_clip_reader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "media/media_clip_ffmpeg.h"
#include "media/media_clip_qtgif.h"
#include "mainwidget.h"
#include "mainwindow.h"

namespace Media {
namespace Clip {
namespace {

QVector<QThread*> threads;
QVector<Manager*> managers;

QPixmap _prepareFrame(const FrameRequest &request, const QImage &original, bool hasAlpha, QImage &cache) {
	bool badSize = (original.width() != request.framew) || (original.height() != request.frameh);
	bool needOuter = (request.outerw != request.framew) || (request.outerh != request.frameh);
	if (badSize || needOuter || hasAlpha || request.rounded) {
		int32 factor(request.factor);
		bool newcache = (cache.width() != request.outerw || cache.height() != request.outerh);
		if (newcache) {
			cache = QImage(request.outerw, request.outerh, QImage::Format_ARGB32_Premultiplied);
			cache.setDevicePixelRatio(factor);
		}
		{
			Painter p(&cache);
			if (newcache) {
				if (request.framew < request.outerw) {
					p.fillRect(0, 0, (request.outerw - request.framew) / (2 * factor), cache.height() / factor, st::black);
					p.fillRect((request.outerw - request.framew) / (2 * factor) + (request.framew / factor), 0, (cache.width() / factor) - ((request.outerw - request.framew) / (2 * factor) + (request.framew / factor)), cache.height() / factor, st::black);
				}
				if (request.frameh < request.outerh) {
					p.fillRect(qMax(0, (request.outerw - request.framew) / (2 * factor)), 0, qMin(cache.width(), request.framew) / factor, (request.outerh - request.frameh) / (2 * factor), st::black);
					p.fillRect(qMax(0, (request.outerw - request.framew) / (2 * factor)), (request.outerh - request.frameh) / (2 * factor) + (request.frameh / factor), qMin(cache.width(), request.framew) / factor, (cache.height() / factor) - ((request.outerh - request.frameh) / (2 * factor) + (request.frameh / factor)), st::black);
				}
			}
			if (hasAlpha) {
				p.fillRect(qMax(0, (request.outerw - request.framew) / (2 * factor)), qMax(0, (request.outerh - request.frameh) / (2 * factor)), qMin(cache.width(), request.framew) / factor, qMin(cache.height(), request.frameh) / factor, st::white);
			}
			QPoint position((request.outerw - request.framew) / (2 * factor), (request.outerh - request.frameh) / (2 * factor));
			if (badSize) {
				p.setRenderHint(QPainter::SmoothPixmapTransform);
				QRect to(position, QSize(request.framew / factor, request.frameh / factor));
				QRect from(0, 0, original.width(), original.height());
				p.drawImage(to, original, from, Qt::ColorOnly);
			} else {
				p.drawImage(position, original);
			}
		}
		if (request.rounded) {
			imageRound(cache);
		}
		return QPixmap::fromImage(cache, Qt::ColorOnly);
	}
	return QPixmap::fromImage(original, Qt::ColorOnly);
}

} // namespace

Reader::Reader(const FileLocation &location, const QByteArray &data, Callback &&callback, Mode mode)
: _callback(std_::move(callback))
, _mode(mode) {
	if (threads.size() < ClipThreadsCount) {
		_threadIndex = threads.size();
		threads.push_back(new QThread());
		managers.push_back(new Manager(threads.back()));
		threads.back()->start();
	} else {
		_threadIndex = int32(rand_value<uint32>() % threads.size());
		int32 loadLevel = 0x7FFFFFFF;
		for (int32 i = 0, l = threads.size(); i < l; ++i) {
			int32 level = managers.at(i)->loadLevel();
			if (level < loadLevel) {
				_threadIndex = i;
				loadLevel = level;
			}
		}
	}
	managers.at(_threadIndex)->append(this, location, data);
}

Reader::Frame *Reader::frameToShow(int32 *index) const { // 0 means not ready
	int32 step = _step.loadAcquire(), i;
	if (step == WaitingForDimensionsStep) {
		if (index) *index = 0;
		return 0;
	} else if (step == WaitingForRequestStep) {
		i = 0;
	} else if (step == WaitingForFirstFrameStep) {
		i = 0;
	} else {
		i = (step / 2) % 3;
	}
	if (index) *index = i;
	return _frames + i;
}

Reader::Frame *Reader::frameToWrite(int32 *index) const { // 0 means not ready
	int32 step = _step.loadAcquire(), i;
	if (step == WaitingForDimensionsStep) {
		i = 0;
	} else if (step == WaitingForRequestStep) {
		if (index) *index = 0;
		return 0;
	} else if (step == WaitingForFirstFrameStep) {
		i = 0;
	} else {
		i = ((step + 2) / 2) % 3;
	}
	if (index) *index = i;
	return _frames + i;
}

Reader::Frame *Reader::frameToWriteNext(bool checkNotWriting, int32 *index) const {
	int32 step = _step.loadAcquire(), i;
	if (step == WaitingForDimensionsStep || step == WaitingForRequestStep || (checkNotWriting && (step % 2))) {
		if (index) *index = 0;
		return 0;
	}
	i = ((step + 4) / 2) % 3;
	if (index) *index = i;
	return _frames + i;
}

void Reader::moveToNextShow() const {
	int32 step = _step.loadAcquire();
	if (step == WaitingForDimensionsStep) {
	} else if (step == WaitingForRequestStep) {
		_step.storeRelease(WaitingForFirstFrameStep);
	} else if (step == WaitingForFirstFrameStep) {
	} else if (!(step % 2)) {
		_step.storeRelease(step + 1);
	}
}

void Reader::moveToNextWrite() const {
	int32 step = _step.loadAcquire();
	if (step == WaitingForDimensionsStep) {
		_step.storeRelease(WaitingForRequestStep);
	} else if (step == WaitingForRequestStep) {
	} else if (step == WaitingForFirstFrameStep) {
		_step.storeRelease(0);
	} else if (step % 2) {
		_step.storeRelease((step + 1) % 6);
	}
}

void Reader::callback(Reader *reader, int32 threadIndex, Notification notification) {
	// check if reader is not deleted already
	if (managers.size() > threadIndex && managers.at(threadIndex)->carries(reader)) {
		reader->_callback.call(notification);
	}
}

void Reader::start(int32 framew, int32 frameh, int32 outerw, int32 outerh, bool rounded) {
	if (managers.size() <= _threadIndex) error();
	if (_state == State::Error) return;

	if (_step.loadAcquire() == WaitingForRequestStep) {
		int factor = cIntRetinaFactor();
		FrameRequest request;
		request.factor = factor;
		request.framew = framew * factor;
		request.frameh = frameh * factor;
		request.outerw = outerw * factor;
		request.outerh = outerh * factor;
		request.rounded = rounded;
		_frames[0].request = _frames[1].request = _frames[2].request = request;
		moveToNextShow();
		managers.at(_threadIndex)->start(this);
	}
}

QPixmap Reader::current(int32 framew, int32 frameh, int32 outerw, int32 outerh, uint64 ms) {
	Frame *frame = frameToShow();
	t_assert(frame != 0);

	if (ms) {
		frame->displayed.storeRelease(1);
		if (_paused.loadAcquire()) {
			_paused.storeRelease(0);
			if (managers.size() <= _threadIndex) error();
			if (_state != State::Error) {
				managers.at(_threadIndex)->update(this);
			}
		}
	} else {
		frame->displayed.storeRelease(-1); // displayed, but should be paused
	}

	int32 factor(cIntRetinaFactor());
	if (frame->pix.width() == outerw * factor && frame->pix.height() == outerh * factor) {
		moveToNextShow();
		return frame->pix;
	}

	frame->request.framew = framew * factor;
	frame->request.frameh = frameh * factor;
	frame->request.outerw = outerw * factor;
	frame->request.outerh = outerh * factor;

	QImage cacheForResize;
	frame->original.setDevicePixelRatio(factor);
	frame->pix = QPixmap();
	frame->pix = _prepareFrame(frame->request, frame->original, true, cacheForResize);

	Frame *other = frameToWriteNext(true);
	if (other) other->request = frame->request;

	moveToNextShow();

	if (managers.size() <= _threadIndex) error();
	if (_state != State::Error) {
		managers.at(_threadIndex)->update(this);
	}

	return frame->pix;
}

bool Reader::ready() const {
	if (_width && _height) return true;

	Frame *frame = frameToShow();
	if (frame) {
		_width = frame->original.width();
		_height = frame->original.height();
		return true;
	}
	return false;
}

int32 Reader::width() const {
	return _width;
}

int32 Reader::height() const {
	return _height;
}

State Reader::state() const {
	return _state;
}

void Reader::stop() {
	if (managers.size() <= _threadIndex) error();
	if (_state != State::Error) {
		managers.at(_threadIndex)->stop(this);
		_width = _height = 0;
	}
}

void Reader::error() {
	_state = State::Error;
}

Reader::~Reader() {
	stop();
}

class ReaderPrivate {
public:
	ReaderPrivate(Reader *reader, const FileLocation &location, const QByteArray &data) : _interface(reader)
	, _mode(reader->mode())
	, _data(data)
	, _location(_data.isEmpty() ? new FileLocation(location) : 0) {
		if (_data.isEmpty() && !_location->accessEnable()) {
			error();
			return;
		}
		_accessed = true;
	}

	ProcessResult start(uint64 ms) {
		if (!_implementation && !init()) {
			return error();
		}
		if (frame() && frame()->original.isNull()) {
			if (!_implementation->readNextFrame()) {
				return error();
			}
			if (!_implementation->renderFrame(frame()->original, frame()->alpha, QSize())) {
				return error();
			}
			_width = frame()->original.width();
			_height = frame()->original.height();
			return ProcessResult::Started;
		}
		return ProcessResult::Wait;
	}

	ProcessResult process(uint64 ms) { // -1 - do nothing, 0 - update, 1 - reinit
		if (_state == State::Error) return ProcessResult::Error;

		if (!_request.valid()) {
			return start(ms);
		}

		if (!_paused && ms >= _nextFrameWhen) {
			return ProcessResult::Repaint;
		}
		return ProcessResult::Wait;
	}

	ProcessResult finishProcess(uint64 ms) {
		if (!readNextFrame()) {
			return error();
		}
		if (ms >= _nextFrameWhen && !readNextFrame(true)) {
			return error();
		}
		if (!renderFrame()) {
			return error();
		}
		return ProcessResult::CopyFrame;
	}

	uint64 nextFrameDelay() {
		int32 delay = _implementation->nextFrameDelay();
		return qMax(delay, 5);
	}

	bool readNextFrame(bool keepup = false) {
		if (!_implementation->readNextFrame()) {
			return false;
		}
		_nextFrameWhen += nextFrameDelay();
		if (keepup) {
			_nextFrameWhen = qMax(_nextFrameWhen, getms());
		}
		return true;
	}

	bool renderFrame() {
		t_assert(frame() != 0 && _request.valid());
		if (!_implementation->renderFrame(frame()->original, frame()->alpha, QSize(_request.framew, _request.frameh))) {
			return false;
		}
		frame()->original.setDevicePixelRatio(_request.factor);
		frame()->pix = QPixmap();
		frame()->pix = _prepareFrame(_request, frame()->original, frame()->alpha, frame()->cache);
		frame()->when = _nextFrameWhen;
		return true;
	}

	bool init() {
		if (_data.isEmpty() && QFileInfo(_location->name()).size() <= AnimationInMemory) {
			QFile f(_location->name());
			if (f.open(QIODevice::ReadOnly)) {
				_data = f.readAll();
				if (f.error() != QFile::NoError) {
					_data = QByteArray();
				}
			}
		}

		_implementation = std_::make_unique<internal::FFMpegReaderImplementation>(_location, &_data);
//		_implementation = new QtGifReaderImplementation(_location, &_data);

		auto implementationMode = [this]() {
			using ImplementationMode = internal::ReaderImplementation::Mode;
			if (_mode == Reader::Mode::Gif) {
				return ImplementationMode::Silent;
			}
			return ImplementationMode::Normal;
		};
		return _implementation->start(implementationMode());
	}

	ProcessResult error() {
		stop();
		_state = State::Error;
		return ProcessResult::Error;
	}

	void stop() {
		_implementation = nullptr;

		if (_location) {
			if (_accessed) {
				_location->accessDisable();
			}
			delete _location;
			_location = 0;
		}
		_accessed = false;
	}

	~ReaderPrivate() {
		stop();
		deleteAndMark(_location);
		_data.clear();
	}

private:
	Reader *_interface;
	State _state = State::Reading;
	Reader::Mode _mode;

	QByteArray _data;
	FileLocation *_location;
	bool _accessed = false;

	QBuffer _buffer;
	std_::unique_ptr<internal::ReaderImplementation> _implementation;

	FrameRequest _request;
	struct Frame {
		QPixmap pix;
		QImage original, cache;
		bool alpha = true;
		uint64 when = 0;
	};
	Frame _frames[3];
	int _frame = 0;
	Frame *frame() {
		return _frames + _frame;
	}

	int _width = 0;
	int _height = 0;

	uint64 _nextFrameWhen = 0;

	bool _paused = false;

	friend class Manager;

};

Manager::Manager(QThread *thread) : _processingInThread(0), _needReProcess(false) {
	moveToThread(thread);
	connect(thread, SIGNAL(started()), this, SLOT(process()));
	connect(thread, SIGNAL(finished()), this, SLOT(finish()));
	connect(this, SIGNAL(processDelayed()), this, SLOT(process()), Qt::QueuedConnection);

	_timer.setSingleShot(true);
	_timer.moveToThread(thread);
	connect(&_timer, SIGNAL(timeout()), this, SLOT(process()));

	anim::registerClipManager(this);
}

void Manager::append(Reader *reader, const FileLocation &location, const QByteArray &data) {
	reader->_private = new ReaderPrivate(reader, location, data);
	_loadLevel.fetchAndAddRelaxed(AverageGifSize);
	update(reader);
}

void Manager::start(Reader *reader) {
	update(reader);
}

void Manager::update(Reader *reader) {
	QReadLocker lock(&_readerPointersMutex);
	auto i = _readerPointers.constFind(reader);
	if (i == _readerPointers.cend()) {
		lock.unlock();

		QWriteLocker lock(&_readerPointersMutex);
		_readerPointers.insert(reader, MutableAtomicInt(1));
	} else {
		i->v.storeRelease(1);
	}
	emit processDelayed();
}

void Manager::stop(Reader *reader) {
	if (!carries(reader)) return;

	QWriteLocker lock(&_readerPointersMutex);
	_readerPointers.remove(reader);
	emit processDelayed();
}

bool Manager::carries(Reader *reader) const {
	QReadLocker lock(&_readerPointersMutex);
	return _readerPointers.contains(reader);
}

Manager::ReaderPointers::iterator Manager::unsafeFindReaderPointer(ReaderPrivate *reader) {
	ReaderPointers::iterator it = _readerPointers.find(reader->_interface);

	// could be a new reader which was realloced in the same address
	return (it == _readerPointers.cend() || it.key()->_private == reader) ? it : _readerPointers.end();
}

Manager::ReaderPointers::const_iterator Manager::constUnsafeFindReaderPointer(ReaderPrivate *reader) const {
	ReaderPointers::const_iterator it = _readerPointers.constFind(reader->_interface);

	// could be a new reader which was realloced in the same address
	return (it == _readerPointers.cend() || it.key()->_private == reader) ? it : _readerPointers.cend();
}

bool Manager::handleProcessResult(ReaderPrivate *reader, ProcessResult result, uint64 ms) {
	QReadLocker lock(&_readerPointersMutex);
	ReaderPointers::const_iterator it = constUnsafeFindReaderPointer(reader);
	if (result == ProcessResult::Error) {
		if (it != _readerPointers.cend()) {
			it.key()->error();
			emit callback(it.key(), it.key()->threadIndex(), NotificationReinit);

			lock.unlock();
			QWriteLocker lock(&_readerPointersMutex);
			ReaderPointers::iterator i = unsafeFindReaderPointer(reader);
			if (i != _readerPointers.cend()) _readerPointers.erase(i);
		}
		return false;
	}
	if (it == _readerPointers.cend()) {
		return false;
	}

	if (result == ProcessResult::Started) {
		_loadLevel.fetchAndAddRelaxed(reader->_width * reader->_height - AverageGifSize);
	}
	if (!reader->_paused && result == ProcessResult::Repaint) {
		int32 ishowing, iprevious;
		Reader::Frame *showing = it.key()->frameToShow(&ishowing), *previous = it.key()->frameToWriteNext(false, &iprevious);
		t_assert(previous != 0 && showing != 0 && ishowing >= 0 && iprevious >= 0);
		if (reader->_frames[ishowing].when > 0 && showing->displayed.loadAcquire() <= 0) { // current frame was not shown
			if (reader->_frames[ishowing].when + WaitBeforeGifPause < ms || (reader->_frames[iprevious].when && previous->displayed.loadAcquire() <= 0)) {
				reader->_paused = true;
				it.key()->_paused.storeRelease(1);
				result = ProcessResult::Paused;
			}
		}
	}
	if (result == ProcessResult::Started || result == ProcessResult::CopyFrame) {
		t_assert(reader->_frame >= 0);
		Reader::Frame *frame = it.key()->_frames + reader->_frame;
		frame->clear();
		frame->pix = reader->frame()->pix;
		frame->original = reader->frame()->original;
		frame->displayed.storeRelease(0);
		if (result == ProcessResult::Started) {
			reader->_nextFrameWhen = ms;
			it.key()->moveToNextWrite();
			emit callback(it.key(), it.key()->threadIndex(), NotificationReinit);
		}
	} else if (result == ProcessResult::Paused) {
		it.key()->moveToNextWrite();
		emit callback(it.key(), it.key()->threadIndex(), NotificationReinit);
	} else if (result == ProcessResult::Repaint) {
		it.key()->moveToNextWrite();
		emit callback(it.key(), it.key()->threadIndex(), NotificationRepaint);
	}
	return true;
}

Manager::ResultHandleState Manager::handleResult(ReaderPrivate *reader, ProcessResult result, uint64 ms) {
	if (!handleProcessResult(reader, result, ms)) {
		_loadLevel.fetchAndAddRelaxed(-1 * (reader->_width > 0 ? reader->_width * reader->_height : AverageGifSize));
		delete reader;
		return ResultHandleRemove;
	}

	_processingInThread->eventDispatcher()->processEvents(QEventLoop::AllEvents);
	if (_processingInThread->isInterruptionRequested()) {
		return ResultHandleStop;
	}

	if (result == ProcessResult::Repaint) {
		{
			QReadLocker lock(&_readerPointersMutex);
			ReaderPointers::const_iterator it = constUnsafeFindReaderPointer(reader);
			if (it != _readerPointers.cend()) {
				int32 index = 0;
				Reader *r = it.key();
				Reader::Frame *frame = it.key()->frameToWrite(&index);
				if (frame) {
					frame->clear();
				} else {
					t_assert(!reader->_request.valid());
				}
				reader->_frame = index;
			}
		}
		return handleResult(reader, reader->finishProcess(ms), ms);
	}

	return ResultHandleContinue;
}

void Manager::process() {
	if (_processingInThread) {
		_needReProcess = true;
		return;
	}

	_timer.stop();
	_processingInThread = thread();

	uint64 ms = getms(), minms = ms + 86400 * 1000ULL;
	{
		QReadLocker lock(&_readerPointersMutex);
		for (auto it = _readerPointers.begin(), e = _readerPointers.end(); it != e; ++it) {
			if (it->v.loadAcquire()) {
				auto i = _readers.find(it.key()->_private);
				if (i == _readers.cend()) {
					_readers.insert(it.key()->_private, 0);
				} else {
					i.value() = ms;
					if (i.key()->_paused && !it.key()->_paused.loadAcquire()) {
						i.key()->_paused = false;
					}
				}
				Reader::Frame *frame = it.key()->frameToWrite();
				if (frame) it.key()->_private->_request = frame->request;
				it->v.storeRelease(0);
			}
		}
	}

	for (auto i = _readers.begin(), e = _readers.end(); i != e;) {
		ReaderPrivate *reader = i.key();
		if (i.value() <= ms) {
			ResultHandleState state = handleResult(reader, reader->process(ms), ms);
			if (state == ResultHandleRemove) {
				i = _readers.erase(i);
				continue;
			} else if (state == ResultHandleStop) {
				_processingInThread = 0;
				return;
			}
			ms = getms();
			i.value() = reader->_nextFrameWhen ? reader->_nextFrameWhen : (ms + 86400 * 1000ULL);
		}
		if (!reader->_paused && i.value() < minms) {
			minms = i.value();
		}
		++i;
	}

	ms = getms();
	if (_needReProcess || minms <= ms) {
		_needReProcess = false;
		_timer.start(1);
	} else {
		_timer.start(minms - ms);
	}

	_processingInThread = 0;
}

void Manager::finish() {
	_timer.stop();
	clear();
}

void Manager::clear() {
	{
		QWriteLocker lock(&_readerPointersMutex);
		for (ReaderPointers::iterator it = _readerPointers.begin(), e = _readerPointers.end(); it != e; ++it) {
			it.key()->_private = 0;
		}
		_readerPointers.clear();
	}

	for (Readers::iterator i = _readers.begin(), e = _readers.end(); i != e; ++i) {
		delete i.key();
	}
	_readers.clear();
}

Manager::~Manager() {
	clear();
}

MTPDocumentAttribute readAttributes(const QString &fname, const QByteArray &data, QImage &cover) {
	FileLocation localloc(StorageFilePartial, fname);
	QByteArray localdata(data);

	auto reader = std_::make_unique<internal::FFMpegReaderImplementation>(&localloc, &localdata);
	if (reader->start(internal::ReaderImplementation::Mode::OnlyGifv)) {
		bool hasAlpha = false;
		if (reader->readNextFrame() && reader->renderFrame(cover, hasAlpha, QSize())) {
			if (cover.width() > 0 && cover.height() > 0 && cover.width() < cover.height() * 10 && cover.height() < cover.width() * 10) {
				if (hasAlpha) {
					QImage cacheForResize;
					FrameRequest request;
					request.framew = request.outerw = cover.width();
					request.frameh = request.outerh = cover.height();
					request.factor = 1;
					cover = _prepareFrame(request, cover, hasAlpha, cacheForResize).toImage();
				}
				int duration = reader->duration();
				return MTP_documentAttributeVideo(MTP_int(duration), MTP_int(cover.width()), MTP_int(cover.height()));
			}
		}
	}
	return MTP_documentAttributeFilename(MTP_string(fname));
}

void Finish() {
	if (!threads.isEmpty()) {
		for (int32 i = 0, l = threads.size(); i < l; ++i) {
			threads.at(i)->quit();
			DEBUG_LOG(("Waiting for clipThread to finish: %1").arg(i));
			threads.at(i)->wait();
			delete managers.at(i);
			delete threads.at(i);
		}
		threads.clear();
		managers.clear();
	}
}

} // namespace Clip
} // namespace Media