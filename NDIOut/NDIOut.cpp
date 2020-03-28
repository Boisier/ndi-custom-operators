//
//  NDIOut.cpp
//  NDIOutTOP
//
//  Created by Valentin Dufois on 2020-03-26.
//  Copyright © 2020 Derivative. All rights reserved.
//

#include "NDIOut.h"

#include "FastMemcpy_Avx.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <cmath>
#include <random>
#include <chrono>

#include <iostream>

NDIOut::NDIOut(const OP_NodeInfo* info)
{
	if(!NDIlib_initialize()) {
		_isErrored = true;
		_errorMessage = "Could not initialized NDI. CPU may be unsupported.";
		return;
	}

	// Set up start and static values
	_GPUDownloadOptions.downloadType = OP_TOPInputDownloadType::Delayed;
	_GPUDownloadOptions.cpuMemPixelType = OP_CPUMemPixelType::BGRA8Fixed;

	_videoFrame.FourCC = NDIlib_FourCC_video_type_e::NDIlib_FourCC_video_type_BGRA;
	_videoFrame.frame_rate_D = 1.0;
	_videoFrame.picture_aspect_ratio = 0;
	_videoFrame.frame_format_type = NDIlib_frame_format_type_progressive;
	_videoFrame.timecode = 0LL;

	_audioFrame.p_data = nullptr;
}

NDIOut::~NDIOut()
{
	NDIlib_send_destroy(_feed);
	NDIlib_destroy();
}

void
NDIOut::getGeneralInfo(TOP_GeneralInfo* ginfo, const OP_Inputs* inputs, void* reserved1)
{
	ginfo->cookEveryFrame = true;
	ginfo->memPixelType = OP_CPUMemPixelType::BGRA8Fixed;
//	ginfo->clearBuffers = false;

	// get parameters
	_params.active = inputs->getParInt("Active");
	_params.fps = inputs->getTimeInfo()->rate;

	std::string parGroups = getGroups(inputs);
	std::string parSourceName = inputs->getParString("Sourcename");

	// Is there already a feed ?
	if(_feed) {
		// Check the feed options againt the user's parameters
		if(!_params.active ||
		   std::string(_feedSettings.p_ndi_name) != parSourceName ||
		   std::string(_feedSettings.p_groups) != parGroups) {
			// Mismatch, end the feed
			NDIlib_send_destroy(_feed);

			_feed = nullptr;
		}
	}

	_params.sourceName = parSourceName;
	_params.groups = parGroups;

	// Do we need to create a feed ?
	if(!_feed && _params.active) {
		_feedSettings.p_ndi_name = _params.sourceName.c_str();
		_feedSettings.p_groups = _params.groups.c_str();
		_feedSettings.clock_audio = false;

		_feed = NDIlib_send_create(&_feedSettings);

		if(!_feed) {
			_isErrored = true;
			_errorMessage = "Could not initialized NDI. CPU may be not supported.";
			return;
		}
	}

	if(!_feed) {
		return;
	}

	// Update the feed metadata
	NDIlib_send_clear_connection_metadata(_feed);

	_feedMetadata.timecode = inputs->getTimeInfo()->absFrame / inputs->getTimeInfo()->rate;

	const OP_DATInput * metadataDAT = inputs->getParDAT("Metadatadat");
	if(metadataDAT) {
		_feedMetadata.p_data = const_cast<char*>(metadataDAT->getCell(0, 0));
		NDIlib_send_add_connection_metadata(_feed, &_feedMetadata);
	}
}

bool
NDIOut::getOutputFormat(TOP_OutputFormat* format, const OP_Inputs* inputs, void* reserved1)
{
	// In this function we could assign variable values to 'format' to specify
	// the pixel format/resolution etc that we want to output to.
	// If we did that, we'd want to return true to tell the TOP to use the settings we've
	// specified.
	// In this example we'll return false and use the TOP's settings

	return false;
}


void
NDIOut::execute(TOP_OutputFormatSpecs* output,
						const OP_Inputs* inputs,
						TOP_Context *context,
						void* reserved1)
{
	if(_isErrored) {
		return;
	}

	// Send audio
	const OP_CHOPInput * audioCHOP = inputs->getParCHOP("Audiochop");
	if(audioCHOP && audioCHOP->numChannels * audioCHOP->numSamples != 0) {
		constexpr size_t floatSize = sizeof(float);

		// Fill the frame
		_audioFrame.sample_rate = audioCHOP->sampleRate;
		_audioFrame.no_channels = audioCHOP->numChannels;
		_audioFrame.no_samples = audioCHOP->numSamples;
		_audioFrame.channel_stride_in_bytes = floatSize * audioCHOP->numSamples;

		_audioFrame.p_data = const_cast<float *>(audioCHOP->channelData[0]);

		// Send
		NDIlib_send_send_audio_v2(_feed, &_audioFrame);
		_audioFrame.p_data = nullptr;
	}

	// Send video
	if(inputs->getNumInputs() != 0) {
		// Get frame data
		const OP_TOPInput * inputTOP = inputs->getInputTOP(0);
		void * inputPtr = inputs->getTOPDataInCPUMemory(inputTOP, &_GPUDownloadOptions);

		// Do we have an input frame ?
		if(inputPtr == nullptr) {
			output->newCPUPixelDataLocation = -1;
			return;
		}

		memcpy_fast(output->cpuPixelData[0], inputPtr, inputTOP->width * inputTOP->height * 4);

		output->newCPUPixelDataLocation = 0;

		if(!_feed) // No feed, no frame
			return;

		// Fill
		_videoFrame.xres = inputTOP->width;
		_videoFrame.yres = inputTOP->height;
		_videoFrame.frame_rate_N = _params.fps;
		_videoFrame.line_stride_in_bytes = inputTOP->width * 4;
		_videoFrame.p_data = (uint8_t*)output->cpuPixelData[0];

#ifdef _WIN32
		NDIlib_send_send_video_v2(_feed, &_videoFrame);
#else
		NDIlib_send_send_video_async_v2(_feed, &_videoFrame);
#endif
	}

}

int32_t NDIOut::getNumInfoCHOPChans(void *reserved1)
{
	// We return the number of channel we want to output to any Info CHOP
	// connected to the TOP. In this example we are just going to send one channel.
	return 1;
}

void NDIOut::getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void* reserved1)
{
	// This function will be called once for each channel we said we'd want to return
	// In this example it'll only be called once.
	chan->name->setString("num_connected");
	chan->value = NDIlib_send_get_no_connections(_feed, 1);
}

bool	NDIOut::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	infoSize->rows = 0;
	infoSize->cols = 0;
	// Setting this to false means we'll be assigning values to the table
	// one row at a time. True means we'll do it one column at a time.
	infoSize->byColumn = false;
	return true;
}

void NDIOut::getInfoDATEntries(int32_t index,
								int32_t nEntries,
								OP_InfoDATEntries* entries,
								void *reserved1)
{

}


// Override these methods if you want to define specfic parameters
void NDIOut::setupParameters(OP_ParameterManager* manager, void* reserved1)
{
	OP_NumericParameter activeToggle;
	activeToggle.name = "Active";
	activeToggle.label = "Active";
	activeToggle.page = "NDI Out";
	manager->appendToggle(activeToggle);

	OP_StringParameter sourceName;
	sourceName.name = "Sourcename";
	sourceName.label = "Source name";
	sourceName.defaultValue = "TouchDesigner";
	sourceName.page = "NDI Out";
	manager->appendString(sourceName);

	OP_StringParameter groups;
	groups.name = "Groupstable";
	groups.label = "Groups Table DAT";
	groups.page = "NDI Out";
	manager->appendDAT(groups);

	OP_StringParameter audioCHOP;
	audioCHOP.name = "Audiochop";
	audioCHOP.label = "Audio CHOP";
	audioCHOP.page = "NDI Out";
	manager->appendCHOP(audioCHOP);

	OP_StringParameter metadataDAT;
	metadataDAT.name = "Metadatadat";
	metadataDAT.label = "Metadata DAT";
	metadataDAT.page = "NDI Out";
	manager->appendDAT(metadataDAT);
}

void NDIOut::pulsePressed(const char* name, void *reserved1)
{

}

void NDIOut::getErrorString(OP_String *error, void *reserved1) {
	if(_isErrored)
		error->setString(_errorMessage.c_str());
}

std::string NDIOut::getGroups(const OP_Inputs* inputs) {
	const OP_DATInput * groupsDAT = inputs->getParDAT("Groupstable");

	// Check if we really need to updates the groups before doing so
	std::string parGroups;

	if(groupsDAT != nullptr &&
	   groupsDAT->isTable && (
							  groupsDAT->opPath != _params.groupsDATPath ||
							  groupsDAT->totalCooks != _params.groupsCookCount
							  )
	   ) {
		_params.groupsDATPath = groupsDAT->opPath;
		_params.groupsCookCount = groupsDAT->totalCooks;

		for(int i = 0; i < groupsDAT->numRows; ++i) {
			for(int j = 0; j < groupsDAT->numCols; ++j) {
				if(strlen(groupsDAT->getCell(i, j)) == 0)
					continue;

				if(parGroups.size() > 0)
					parGroups += ",";

				parGroups += groupsDAT->getCell(i, j);
			}
		}
	}

	return parGroups;
}
