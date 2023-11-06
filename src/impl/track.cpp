/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "track.hpp"
#include "internals.hpp"
#include "logcounter.hpp"
#include "peerconnection.hpp"
#include "rtp.hpp"

namespace rtc::impl {

static LogCounter COUNTER_MEDIA_BAD_DIRECTION(plog::warning,
                                              "Number of media packets sent in invalid directions");
static LogCounter COUNTER_QUEUE_FULL(plog::warning,
                                     "Number of media packets dropped due to a full queue");

Track::Track(weak_ptr<PeerConnection> pc, Description::Media desc)
    : mPeerConnection(pc),
      mRecvQueue(RECV_QUEUE_LIMIT, [](const message_ptr &m) { return m->size(); }) {

	setDescription(std::move(desc));

	// Discard messages by default if track is send only
	if (mMediaDescription.direction() == Description::Direction::SendOnly)
		messageCallback = [](message_variant) {};
}

Track::~Track() {
	PLOG_VERBOSE << "Destroying Track";
	try {
		close();
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
	}
}

string Track::mid() const {
	std::shared_lock lock(mMutex);
	return mMediaDescription.mid();
}

Description::Direction Track::direction() const {
	std::shared_lock lock(mMutex);
	return mMediaDescription.direction();
}

Description::Media Track::description() const {
	std::shared_lock lock(mMutex);
	return mMediaDescription;
}

void Track::setDescription(Description::Media desc) {
	{
		std::unique_lock lock(mMutex);
		if (desc.mid() != mMediaDescription.mid())
			throw std::logic_error("Media description mid does not match track mid");

		// RFC 8843: The RTP MID header extension MUST be enabled, by including an SDP 'extmap'
		// attribute [RFC8285], with a 'urn:ietf:params:rtp-hdrext:sdes:mid' URI value, in each
		// bundled RTP-based "m=" section in every offer and answer.
		const string sdesMidExtUri = "urn:ietf:params:rtp-hdrext:sdes:mid";
		mSdesMidExtId = desc.findExtId(sdesMidExtUri);
		if (!mSdesMidExtId) {
			mSdesMidExtId.emplace(desc.nextExtId());
			desc.addExtMap({*mSdesMidExtId, sdesMidExtUri});
		}

		mMediaDescription = std::move(desc);
	}

	if (auto handler = getMediaHandler())
		handler->media(description());
}

void Track::close() {
	PLOG_VERBOSE << "Closing Track";

	if (!mIsClosed.exchange(true))
		triggerClosed();

	setMediaHandler(nullptr);
	resetCallbacks();
}

optional<message_variant> Track::receive() {
	if (auto next = mRecvQueue.pop()) {
		message_ptr message = *next;
		if (message->type == Message::Control)
			return to_variant(**next); // The same message may be frowarded into multiple Tracks
		else
			return to_variant(std::move(*message));
	}
	return nullopt;
}

optional<message_variant> Track::peek() {
	if (auto next = mRecvQueue.peek()) {
		message_ptr message = *next;
		if (message->type == Message::Control)
			return to_variant(**next); // The same message may be forwarded into multiple Tracks
		else
			return to_variant(std::move(*message));
	}
	return nullopt;
}

size_t Track::availableAmount() const { return mRecvQueue.amount(); }

bool Track::isOpen(void) const {
#if RTC_ENABLE_MEDIA
	std::shared_lock lock(mMutex);
	return !mIsClosed && mDtlsSrtpTransport.lock();
#else
	return false;
#endif
}

bool Track::isClosed(void) const { return mIsClosed; }

size_t Track::maxMessageSize() const {
	optional<size_t> mtu;
	if (auto pc = mPeerConnection.lock())
		mtu = pc->config.mtu;

	return mtu.value_or(DEFAULT_MTU) - 12 - 8 - 40; // SRTP/UDP/IPv6
}

#if RTC_ENABLE_MEDIA
void Track::open(shared_ptr<DtlsSrtpTransport> transport) {
	{
		std::lock_guard lock(mMutex);
		mDtlsSrtpTransport = transport;
	}

	if (!mIsClosed)
		triggerOpen();
}
#endif

void Track::incoming(message_ptr message) {
	if (!message)
		return;

	auto dir = direction();
	if ((dir == Description::Direction::SendOnly || dir == Description::Direction::Inactive) &&
	    message->type != Message::Control) {
		COUNTER_MEDIA_BAD_DIRECTION++;
		return;
	}

	message_vector messages{std::move(message)};
	if (auto handler = getMediaHandler())
		handler->incomingChain(messages, [this](message_ptr m) { transportSend(m); });

	for (auto &m : messages) {
		// Tail drop if queue is full
		if (mRecvQueue.full()) {
			COUNTER_QUEUE_FULL++;
			return;
		}

		mRecvQueue.push(m);
		triggerAvailable(mRecvQueue.size());
	}
}

bool Track::outgoing(message_ptr message) {
	if (mIsClosed)
		throw std::runtime_error("Track is closed");

	auto handler = getMediaHandler();

	// If there is no handler, the track expects RTP or RTCP packets
	if (!handler && IsRtcp(*message))
		message->type = Message::Control; // to allow sending RTCP packets irrelevant of direction

	auto dir = direction();
	if ((dir == Description::Direction::RecvOnly || dir == Description::Direction::Inactive) &&
	    message->type != Message::Control) {
		COUNTER_MEDIA_BAD_DIRECTION++;
		return false;
	}

	if (handler) {
		message_vector messages{std::move(message)};
		handler->outgoingChain(messages, [this](message_ptr m) { transportSend(m); });
		bool ret = false;
		for (auto &m : messages)
			ret = transportSend(std::move(m));

		return ret;

	} else {
		return transportSend(std::move(message));
	}
}

bool Track::transportSend([[maybe_unused]] message_ptr message) {
#if RTC_ENABLE_MEDIA
	shared_ptr<DtlsSrtpTransport> transport;
	{
		std::shared_lock lock(mMutex);
		transport = mDtlsSrtpTransport.lock();
		if (!transport)
			throw std::runtime_error("Track is closed");

		// Set recommended medium-priority DSCP value
		// See https://www.rfc-editor.org/rfc/rfc8837.html#section-5
		if (mMediaDescription.type() == "audio")
			message->dscp = 46; // EF: Expedited Forwarding
		else
			message->dscp = 36; // AF42: Assured Forwarding class 4, medium drop probability
	}

	return transport->sendMedia(message);
#else
	throw std::runtime_error("Track is disabled (not compiled with media support)");
#endif
}

void Track::setMediaHandler(shared_ptr<MediaHandler> handler) {
	{
		std::unique_lock lock(mMutex);
		mMediaHandler = handler;
	}

	if (handler)
		handler->media(description());
}

shared_ptr<MediaHandler> Track::getMediaHandler() {
	std::shared_lock lock(mMutex);
	return mMediaHandler;
}


void Track::tagWithMid(message_ptr message) {
	// TODO: lock
	if(IsRtcp(*message)) {

	} else {
		if(!mSdesMidExtId)
			return;

		if(message->size() < sizeof(RtpHeader)) {
			const string mid =mMediaDescription.mid();
			const size_t sdesMidExtLength = 1 + mid.size();

			auto *header = reinterpret_cast<RtpHeader*>(message->data());
			if(header->extension()) {
				auto *extHeader = header->getExtensionHeader();
				if(extHeader->profileSpecificId() != 0xbede) // TODO: check in RtpHeader
					return;

				size_t extHeaderLength = extHeader->headerLength(); // TODO: actually used length
				size_t newExtHeaderLength = (extHeaderLength + sdesMidExtLength + 3) / 4;
				message->insert(message->begin() + header->getSize(), newExtHeaderLength - extHeaderLength, byte(0));
				extHeader->setHeaderLength(static_cast<uint16_t>(newExtHeaderLength));
				extHeader->writeOneByteHeader(extHeaderLength, *mSdesMidExtId, reinterpret_cast<const std::byte *>(mid.c_str()), mid.size());
			} else {
				size_t extHeaderLength = (sdesMidExtLength + 3) / 4;
				message->insert(message->begin() + header->getSize(), sizeof(RtpExtensionHeader) + extHeaderLength, byte(0));
				header->setExtension(true);
				auto *extHeader = header->getExtensionHeader();
				extHeader->setProfileSpecificId(0xbede);
				extHeader->setHeaderLength(static_cast<uint16_t>(extHeaderLength));
				extHeader->writeOneByteHeader(0, *mSdesMidExtId, reinterpret_cast<const std::byte *>(mid.c_str()), mid.size());
			}
		}
	}
}

} // namespace rtc::impl
