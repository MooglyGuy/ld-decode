/************************************************************************

    f2framestoaudio.cpp

    ld-process-efm - EFM data decoder
    Copyright (C) 2019 Simon Inns

    This file is part of ld-decode-tools.

    ld-process-efm is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#include "JsonWax/JsonWax.h"
#include "f2framestoaudio.h"
#include "logging.h"

F2FramesToAudio::F2FramesToAudio()
{
    reset();
}

// Method to reset and flush all buffers
void F2FramesToAudio::reset(void)
{
    resetStatistics();
}

// Methods to handle statistics
void F2FramesToAudio::resetStatistics(void)
{
    statistics.validAudioSamples = 0;
    statistics.invalidAudioSamples = 0;
    statistics.sectionsProcessed = 0;
    statistics.encoderRunning = 0;
    statistics.encoderStopped = 0;
    statistics.qModeICount = 0;
    statistics.trackNumber = 0;
    statistics.subdivision = 0;
    statistics.discTime.setTime(0, 0, 0);
    statistics.trackTime.setTime(0, 0, 0);

    // Subcode block Q mode counters
    statistics.qMode1Count = 0;
    statistics.qMode4Count = 0;
}

F2FramesToAudio::Statistics F2FramesToAudio::getStatistics(void)
{
    return statistics;
}

// Method to write status information to qCInfo
void F2FramesToAudio::reportStatus(void)
{
    qInfo() << "F2 Frames to audio converter:";
    qInfo() << "  Valid audio samples =" << statistics.validAudioSamples;
    qInfo() << "  Invalid audio samples =" << statistics.invalidAudioSamples;
    qInfo() << "  Sections processed =" << statistics.sectionsProcessed;
    qInfo() << "  Encoder running sections =" << statistics.encoderRunning;
    qInfo() << "  Encoder stopped sections =" << statistics.encoderStopped;

    qInfo() << "  Q Mode 1 sections =" << statistics.qMode1Count << "(CD Audio)";
    qInfo() << "  Q Mode 4 sections =" << statistics.qMode4Count << "(LD Audio)";
    qInfo() << "  Q Mode invalid sections =" << statistics.qModeICount;
}

// Method to set the audio output file
bool F2FramesToAudio::setOutputFile(QFile *outputFileHandle)
{
    // Open output file for writing
    this->outputFileHandle = outputFileHandle;

    // Exit with success
    return true;
}

// Convert F2 frames into audio sample data
void F2FramesToAudio::convert(QVector<F2Frame> f2Frames, QVector<Section> sections)
{
    // Note: At a sample rate of 44100Hz there are 44,100 samples per second
    // There are 75 sections per second
    // Therefore there are 588 samples per section

    // Each F2 frame contains 24 bytes and there are 4 bytes per stereo sample pair
    // therefore each F2 contains 6 samples
    // therefore there are 98 F2 frames per section
    f2FramesIn.append(f2Frames);
    sectionsIn.append(sections);

    // Do we have enough data to output audio information?
    if (f2FramesIn.size() >= 98 && sectionsIn.size() >= 1) processAudio();
}

// NOTE: keep track of the elapsed time by number of samples (independent of the sections etc)

void F2FramesToAudio::processAudio(void)
{
    qint32 f2FrameNumber = 0;
    qint32 sectionsToProcess = f2FramesIn.size() / 98;
    if (sectionsIn.size() < sectionsToProcess) sectionsToProcess = sectionsIn.size();

    // Process one section of audio at a time (98 F2 Frames)
    for (qint32 sectionNo = 0; sectionNo < sectionsToProcess; sectionNo++) {
        // Get the required metadata for processing from the section
        Metadata metadata = sectionToMeta(sectionsIn[sectionNo]);

        // Probably should verify Q Mode is audio here? (and isAudio flag...)

        // Output the samples to file (98 f2 frames x 6 samples per frame = 588)
        for (qint32 i = f2FrameNumber; i < f2FrameNumber + 98; i++) {
            if (metadata.encoderRunning) {
                // Encoder running, output audio samples

                // Check F2 Frame data payload validity
                if (!f2FramesIn[i].getDataValid()) {
                    // F2 Frame data has errors - 6 samples might be garbage
                    statistics.invalidAudioSamples += 6;
                } else {
                    statistics.validAudioSamples += 6; // 24 bytes per F2 (/2 = 16-bit and /2 = stereo)
                }

                // Encoder running, output samples
                outputFileHandle->write(f2FramesIn[i].getDataSymbols()); // 24 bytes per F2
            } else {
                // Encoder stopped, output F2 frame's worth in zeros
                QByteArray dummy;
                dummy.fill(0, 24);
                outputFileHandle->write(dummy);
            }
        }
        f2FrameNumber += 98;

        statistics.sectionsProcessed++;
    }

    // Remove processed F2Frames and samples from buffer
    f2FramesIn.remove(0, sectionsToProcess * 98);
    sectionsIn.remove(0, sectionsToProcess);
}

// Metadata processing ------------------------------------------------------------------------------------------------

// Method to open the metadata output file
bool F2FramesToAudio::setMetadataOutputFile(QFile *outputMetadataFileHandle)
{
    // Open output file for writing
    this->outputMetadataFileHandle = outputMetadataFileHandle;

    // Here we just store the required filename
    // The file is created and filled on close
    jsonFilename = outputMetadataFileHandle->fileName();

    // Exit with success
    return true;
}

// Method to flush the metadata to the output file
void F2FramesToAudio::flushMetadata(void)
{
    // Define the JSON object
    JsonWax json;

    // Write out the entries
    for (qint32 subcodeNo = 0; subcodeNo < qMetaDataVector.size(); subcodeNo++) {
        // Process entry
        json.setValue({"subcode", subcodeNo, "seqNo"}, subcodeNo);

        if (qMetaModeVector[subcodeNo] == 1) {
            // Q-Mode 1 - CD audio
            json.setValue({"subcode", subcodeNo, "qControl", "isAudio"}, qMetaDataVector[subcodeNo].qControl.isAudioNotData);
            json.setValue({"subcode", subcodeNo, "qControl", "isStereo"}, qMetaDataVector[subcodeNo].qControl.isStereoNotQuad);
            json.setValue({"subcode", subcodeNo, "qControl", "isNoPreemp"}, qMetaDataVector[subcodeNo].qControl.isNoPreempNotPreemp);
            json.setValue({"subcode", subcodeNo, "qControl", "isCopyProtected"}, qMetaDataVector[subcodeNo].qControl.isCopyProtectedNotUnprotected);

            json.setValue({"subcode", subcodeNo, "qData", "qMode"}, qMetaModeVector[subcodeNo]);
            json.setValue({"subcode", subcodeNo, "qData", "isLeadIn"}, qMetaDataVector[subcodeNo].qMode1.isLeadIn);
            json.setValue({"subcode", subcodeNo, "qData", "isLeadOut"}, qMetaDataVector[subcodeNo].qMode1.isLeadOut);
            json.setValue({"subcode", subcodeNo, "qData", "trackNumber"}, qMetaDataVector[subcodeNo].qMode1.trackNumber);
            json.setValue({"subcode", subcodeNo, "qData", "point"}, qMetaDataVector[subcodeNo].qMode1.point);
            json.setValue({"subcode", subcodeNo, "qData", "x"}, qMetaDataVector[subcodeNo].qMode1.x);
            json.setValue({"subcode", subcodeNo, "qData", "trackTime"}, qMetaDataVector[subcodeNo].qMode1.trackTime.getTimeAsQString());
            json.setValue({"subcode", subcodeNo, "qData", "discTime"}, qMetaDataVector[subcodeNo].qMode1.discTime.getTimeAsQString());
        } else if (qMetaModeVector[subcodeNo] == 4) {
            // Q-Mode 4 - LD Audio
            json.setValue({"subcode", subcodeNo, "qControl", "isAudio"}, qMetaDataVector[subcodeNo].qControl.isAudioNotData);
            json.setValue({"subcode", subcodeNo, "qControl", "isStereo"}, qMetaDataVector[subcodeNo].qControl.isStereoNotQuad);
            json.setValue({"subcode", subcodeNo, "qControl", "isNoPreemp"}, qMetaDataVector[subcodeNo].qControl.isNoPreempNotPreemp);
            json.setValue({"subcode", subcodeNo, "qControl", "isCopyProtected"}, qMetaDataVector[subcodeNo].qControl.isCopyProtectedNotUnprotected);

            json.setValue({"subcode", subcodeNo, "qData", "qMode"}, qMetaModeVector[subcodeNo]);
            json.setValue({"subcode", subcodeNo, "qData", "isLeadIn"}, qMetaDataVector[subcodeNo].qMode4.isLeadIn);
            json.setValue({"subcode", subcodeNo, "qData", "isLeadOut"}, qMetaDataVector[subcodeNo].qMode4.isLeadOut);
            json.setValue({"subcode", subcodeNo, "qData", "trackNumber"}, qMetaDataVector[subcodeNo].qMode4.trackNumber);
            json.setValue({"subcode", subcodeNo, "qData", "point"}, qMetaDataVector[subcodeNo].qMode4.point);
            json.setValue({"subcode", subcodeNo, "qData", "x"}, qMetaDataVector[subcodeNo].qMode4.x);
            json.setValue({"subcode", subcodeNo, "qData", "trackTime"}, qMetaDataVector[subcodeNo].qMode4.trackTime.getTimeAsQString());
            json.setValue({"subcode", subcodeNo, "qData", "discTime"}, qMetaDataVector[subcodeNo].qMode4.discTime.getTimeAsQString());
        } else {
            // Unknown Q Mode / Non-Audio Q Mode
            json.setValue({"subcode", subcodeNo, "qControl", "isAudio"}, qMetaDataVector[subcodeNo].qControl.isAudioNotData);
            json.setValue({"subcode", subcodeNo, "qControl", "isStereo"}, qMetaDataVector[subcodeNo].qControl.isStereoNotQuad);
            json.setValue({"subcode", subcodeNo, "qControl", "isNoPreemp"}, qMetaDataVector[subcodeNo].qControl.isNoPreempNotPreemp);
            json.setValue({"subcode", subcodeNo, "qControl", "isCopyProtected"}, qMetaDataVector[subcodeNo].qControl.isCopyProtectedNotUnprotected);

            json.setValue({"subcode", subcodeNo, "qData", "qMode"}, qMetaModeVector[subcodeNo]);
        }
    }

    // Write the JSON object to file
    qDebug() << "SectionToMeta::closeOutputFile(): Writing JSON metadata file";
    if (!json.saveAs(jsonFilename, JsonWax::Readable)) {
        qCritical("Writing JSON file failed!");
        return;
    }
}

// Method to process as section into audio metadata
F2FramesToAudio::Metadata F2FramesToAudio::sectionToMeta(Section section)
{
    Metadata metadata;

    // Get the Q Control
    if (section.getQMetadata().qControl.isAudioNotData) metadata.isAudio = true;
    else metadata.isAudio = false;

    // Get the Q Mode
    metadata.qMode = section.getQMode();

    // Store the metadata (for the flush JSON operation)
    Section::QMetadata qMetaData = section.getQMetadata();
    qMetaModeVector.append(metadata.qMode);
    qMetaDataVector.append(qMetaData);

    // Depending on the section Q Mode, process the section
    if (metadata.qMode == 1) {
        // CD Audio
        statistics.qMode1Count++;

        if (qMetaData.qMode1.isLeadIn) {
            // Q Mode 1 - Lead in section
            metadata.trackNumber = qMetaData.qMode1.trackNumber;
            metadata.subdivision = qMetaData.qMode1.point;
            metadata.trackTime = qMetaData.qMode1.trackTime;
            metadata.discTime = qMetaData.qMode1.discTime;
            metadata.encoderRunning = false;
            metadata.isCorrected = false;
        } else if (qMetaData.qMode1.isLeadOut) {
            // Q Mode 1 - Lead out section
            if (qMetaData.qMode1.x == 0) {
                // Encoding paused
                metadata.trackNumber = qMetaData.qMode1.trackNumber;
                metadata.subdivision = 0;
                metadata.trackTime = qMetaData.qMode1.trackTime;
                metadata.discTime = qMetaData.qMode1.discTime;
                metadata.encoderRunning = false;
                metadata.isCorrected = false;
            } else {
                // Encoding running
                metadata.trackNumber = qMetaData.qMode1.trackNumber;
                metadata.subdivision = 0;
                metadata.trackTime = qMetaData.qMode1.trackTime;
                metadata.discTime = qMetaData.qMode1.discTime;
                metadata.encoderRunning = true;
                metadata.isCorrected = false;
            }
        } else {
            // Q Mode 1 - Audio section
            if (qMetaData.qMode1.x == 0) {
                // Encoding paused
                metadata.trackNumber = qMetaData.qMode1.trackNumber;
                metadata.subdivision = qMetaData.qMode1.x;
                metadata.trackTime = qMetaData.qMode1.trackTime;
                metadata.discTime = qMetaData.qMode1.discTime;
                metadata.encoderRunning = false;
                metadata.isCorrected = false;
            } else {
                // Encoding running
                metadata.trackNumber = qMetaData.qMode1.trackNumber;
                metadata.subdivision = qMetaData.qMode1.x;
                metadata.trackTime = qMetaData.qMode1.trackTime;
                metadata.discTime = qMetaData.qMode1.discTime;
                metadata.encoderRunning = true;
                metadata.isCorrected = false;
            }
        }
    } else if (metadata.qMode == 4) {
        // 4 = non-CD Audio (LaserDisc)
        statistics.qMode4Count++;

        if (qMetaData.qMode4.isLeadIn) {
            // Q Mode 4 - Lead in section
            metadata.trackNumber = qMetaData.qMode4.trackNumber;
            metadata.subdivision = qMetaData.qMode4.point;
            metadata.trackTime = qMetaData.qMode4.trackTime;
            metadata.discTime = qMetaData.qMode4.discTime;
            metadata.encoderRunning = false;
            metadata.isCorrected = false;
        } else if (qMetaData.qMode4.isLeadOut) {
            // Q Mode 4 - Lead out section
            if (qMetaData.qMode4.x == 0) {
                // Encoding paused
                metadata.trackNumber = qMetaData.qMode4.trackNumber;
                metadata.subdivision = 0;
                metadata.trackTime = qMetaData.qMode4.trackTime;
                metadata.discTime = qMetaData.qMode4.discTime;
                metadata.encoderRunning = false;
                metadata.isCorrected = false;
            } else {
                // Encoding running
                metadata.trackNumber = qMetaData.qMode4.trackNumber;
                metadata.subdivision = 0;
                metadata.trackTime = qMetaData.qMode4.trackTime;
                metadata.discTime = qMetaData.qMode4.discTime;
                metadata.encoderRunning = true;
                metadata.isCorrected = false;
            }
        } else {
            // Q Mode 4 - Audio section
            if (qMetaData.qMode4.x == 0) {
                // Encoding paused
                metadata.trackNumber = qMetaData.qMode4.trackNumber;
                metadata.subdivision = qMetaData.qMode4.x;
                metadata.trackTime = qMetaData.qMode4.trackTime;
                metadata.discTime = qMetaData.qMode4.discTime;
                metadata.encoderRunning = false;
                metadata.isCorrected = false;
            } else {
                // Encoding running
                metadata.trackNumber = qMetaData.qMode4.trackNumber;
                metadata.subdivision = qMetaData.qMode4.x;
                metadata.trackTime = qMetaData.qMode4.trackTime;
                metadata.discTime = qMetaData.qMode4.discTime;
                metadata.encoderRunning = true;
                metadata.isCorrected = false;
            }
        }
    } else {
        // Invalid section / Non-audio Q Mode
        statistics.qModeICount++;

        metadata.trackNumber = -1;
        metadata.subdivision = -1;
        metadata.trackTime.setTime(0, 0, 0);
        metadata.discTime.setTime(0, 0, 0);
        metadata.encoderRunning = true; // Perhaps should default to false?
        metadata.isCorrected = false;
    }

    // Update statistics
    statistics.discTime = metadata.discTime;
    statistics.trackTime = metadata.trackTime;
    statistics.subdivision = metadata.subdivision;
    statistics.trackNumber = metadata.trackNumber;

    if (metadata.encoderRunning) statistics.encoderRunning++;
    else statistics.encoderStopped++;

    return metadata;
}
