/*
 *   Copyright (C) 2020,2021,2023,2024 by Jonathan Naylor G4KLX
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "FMRAWNetwork.h"
#include "Defines.h"
#include "Utils.h"
#include "Log.h"

#include <cstdio>
#include <cassert>
#include <cstring>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

const unsigned int MMDVM_SAMPLERATE = 8000U;

const unsigned int BUFFER_LENGTH = 1500U;

CFMRAWNetwork::CFMRAWNetwork(const std::string& localAddress, unsigned short localPort, const std::string& gatewayAddress, unsigned short gatewayPort, unsigned int sampleRate, const std::string& squelchFile, bool debug) :
m_socket(localAddress, localPort),
m_addr(),
m_addrLen(0U),
m_sampleRate(sampleRate),
m_squelchFile(squelchFile),
m_debug(debug),
m_enabled(false),
m_buffer(2000U, "FM Network"),
m_seqNo(0U),
m_resampler(NULL),
m_error(0),
m_fd(-1)
{
	assert(gatewayPort > 0U);
	assert(!gatewayAddress.empty());
	assert(sampleRate > 0U);

	if (CUDPSocket::lookup(gatewayAddress, gatewayPort, m_addr, m_addrLen) != 0)
		m_addrLen = 0U;

	m_resampler = ::src_new(SRC_SINC_FASTEST, 1, &m_error);
}

CFMRAWNetwork::~CFMRAWNetwork()
{
	::src_delete(m_resampler);
}

bool CFMRAWNetwork::open()
{
	if (m_addrLen == 0U) {
		LogError("Unable to resolve the address of the FM Gateway");
		return false;
	}

	LogMessage("Opening FM RAW network connection");

	if (!m_squelchFile.empty()) {
		m_fd = ::open(m_squelchFile.c_str(), O_WRONLY | O_SYNC);
		if (m_fd == -1) {
			LogError("Cannot open the squelch file: %s, errno=%d", m_squelchFile.c_str(), errno);
			return false;
		}
	}

	return m_socket.open(m_addr);
}

bool CFMRAWNetwork::writeData(const float* in, unsigned int nIn)
{
	assert(in != NULL);
	assert(nIn > 0U);

	if (m_seqNo == 0U) {
		bool ret = writeStart();
		if (!ret)
			return false;
	}

	unsigned char buffer[2000U];

	unsigned int length = 0U;

	if (m_sampleRate != MMDVM_SAMPLERATE) {
		unsigned int nOut = (nIn * m_sampleRate) / MMDVM_SAMPLERATE;

		float out[1000U];

		SRC_DATA data;
		data.data_in       = in;
		data.data_out      = out;
		data.input_frames  = nIn;
		data.output_frames = nOut;
		data.end_of_input  = 0;
		data.src_ratio     = float(m_sampleRate) / float(MMDVM_SAMPLERATE);

		int ret = ::src_process(m_resampler, &data);
		if (ret != 0) {
			LogError("Error from the write resampler - %d - %s", ret, ::src_strerror(ret));
			return false;
		}

		for (unsigned int i = 0U; i < nOut; i++) {
			short val = short(out[i] * 32767.0F + 0.5F);	// Changing audio format from float to S16LE

			buffer[length++] = (val >> 0) & 0xFFU;
			buffer[length++] = (val >> 8) & 0xFFU;
		}
	} else {
		for (unsigned int i = 0U; i < nIn; i++) {
			short val = short(in[i] * 32767.0F + 0.5F);	// Changing audio format from float to S16LE

			buffer[length++] = (val >> 0) & 0xFFU;
			buffer[length++] = (val >> 8) & 0xFFU;
		}
	}

	if (m_debug)
		CUtils::dump(1U, "FM RAW Network Data Sent", buffer, length);

	m_seqNo++;

	return m_socket.write(buffer, length, m_addr, m_addrLen);
}

bool CFMRAWNetwork::writeEnd()
{
	m_seqNo = 0U;

	if (m_fd != -1) {
		size_t n = ::write(m_fd, "Z", 1);
		if (n != 1) {
			LogError("Cannot write to the squelch file: %s, errno=%d", m_squelchFile.c_str(), errno);
			return false;
		}
	}

	return true;
}

void CFMRAWNetwork::clock(unsigned int ms)
{
	unsigned char buffer[BUFFER_LENGTH];

	sockaddr_storage addr;
	unsigned int addrlen;
	int length = m_socket.read(buffer, BUFFER_LENGTH, addr, addrlen);
	if (length <= 0)
		return;

	if (!CUDPSocket::match(addr, m_addr, IMT_ADDRESS_ONLY)) {
		LogMessage("FM RAW packet received from an invalid source");
		return;
	}

	if (!m_enabled)
		return;

	if (m_debug)
		CUtils::dump(1U, "FM RAW Network Data Received", buffer, length);

	m_buffer.addData(buffer, length);
}

unsigned int CFMRAWNetwork::readData(float* out, unsigned int nOut)
{
	assert(out != NULL);
	assert(nOut > 0U);

	unsigned int bytes = m_buffer.dataSize() / sizeof(unsigned short);
	if (bytes == 0U)
		return 0U;

	if (m_sampleRate != MMDVM_SAMPLERATE) {
		unsigned int nIn = (nOut * m_sampleRate) / MMDVM_SAMPLERATE;

		if (bytes < nIn) {
			nIn = bytes;
			nOut = (nIn * MMDVM_SAMPLERATE) / m_sampleRate;
		}

		unsigned char buffer[2000U];
		m_buffer.getData(buffer, nIn * sizeof(unsigned short));

		float in[1000U];

		for (unsigned int i = 0U; i < nIn; i++) {
			short val = ((buffer[i * 2U + 0U] & 0xFFU) << 0) + ((buffer[i * 2U + 1U] & 0xFFU) << 8);
			in[i] = float(val) / 65536.0F;
		}

		SRC_DATA data;
		data.data_in       = in;
		data.data_out      = out;
		data.input_frames  = nIn;
		data.output_frames = nOut;
		data.end_of_input  = 0;
		data.src_ratio     = float(MMDVM_SAMPLERATE) / float(m_sampleRate);

		int ret = ::src_process(m_resampler, &data);
		if (ret != 0) {
			LogError("Error from the read resampler - %d - %s", ret, ::src_strerror(ret));
			return false;
		}
	} else {
		if (bytes < nOut)
			nOut = bytes;

		unsigned char buffer[1500U];
		m_buffer.getData(buffer, nOut * sizeof(unsigned short));

		for (unsigned int i = 0U; i < nOut; i++) {
			short val = ((buffer[i * 2U + 0U] & 0xFFU) << 0) + ((buffer[i * 2U + 1U] & 0xFFU) << 8);
			out[i] = float(val) / 65536.0F;
		}
	}

	return nOut;
}

void CFMRAWNetwork::reset()
{
	m_buffer.clear();
}

void CFMRAWNetwork::close()
{
	m_socket.close();

	if (m_fd != -1) {
		::close(m_fd);
		m_fd = -1;
	}

	LogMessage("Closing FM RAW network connection");
}

void CFMRAWNetwork::enable(bool enabled)
{
	if (enabled && !m_enabled)
		reset();
	else if (!enabled && m_enabled)
		reset();

	m_enabled = enabled;
}

bool CFMRAWNetwork::writeStart()
{
	if (m_fd != -1) {
		size_t n = ::write(m_fd, "O", 1);
		if (n != 1) {
			LogError("Cannot write to the squelch file: %s, errno=%d", m_squelchFile.c_str(), errno);
			return false;
		}
	}

	return true;
}
