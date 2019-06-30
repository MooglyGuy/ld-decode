/************************************************************************

    c1circ.cpp

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

#include "c1circ.h"

C1Circ::C1Circ()
{
    currentF3Data.resize(32);
    previousF3Data.resize(32);
    currentF3Errors.resize(32);
    previousF3Errors.resize(32);

    interleavedC1Data.resize(32);
    interleavedC1Errors.resize(32);

    outputC1Data.resize(28);
    outputC1Errors.resize(28);

    reset();
}

// Method to reset and flush all buffers
void C1Circ::reset(void)
{
    flush();
    resetStatistics();
}

// Methods to handle statistics
void C1Circ::resetStatistics(void)
{
    statistics.c1Passed = 0;
    statistics.c1Corrected = 0;
    statistics.c1Failed = 0;
    statistics.c1flushed = 0;
}

C1Circ::Statistics C1Circ::getStatistics(void)
{
    return statistics;
}

// Method to write statistics information to qInfo
void C1Circ::reportStatistics(void)
{
    qInfo() << "";
    qInfo() << "F3 to F2 frame C1 Error correction:";
    qInfo() << "  Total C1s processed:" << statistics.c1Passed + statistics.c1Corrected + statistics.c1Failed;
    qInfo() << "            Valid C1s:" << statistics.c1Passed + statistics.c1Corrected;
    qInfo() << "          Invalid C1s:" << statistics.c1Failed;
    qInfo() << "        C1s corrected:" << statistics.c1Corrected;
    qInfo() << " Delay buffer flushes:" << statistics.c1flushed;
}

void C1Circ::pushF3Frame(F3Frame f3Frame)
{
    previousF3Data = currentF3Data;
    currentF3Data = f3Frame.getDataSymbols();

    previousF3Errors = currentF3Errors;
    currentF3Errors = f3Frame.getErrorSymbols();

    c1BufferLevel++;
    if (c1BufferLevel > 1) {
        c1BufferLevel = 2;

        // Interleave the F3 data and perform C1 error correction
        interleave();
        errorCorrect();
    }
}

// Return the C1 data symbols if available
QByteArray C1Circ::getDataSymbols(void)
{
    if (c1BufferLevel > 1) return outputC1Data;
    return QByteArray();
}

// Return the C1 error symbols if available
QByteArray C1Circ::getErrorSymbols(void)
{
    if (c1BufferLevel > 1) return outputC1Errors;
    return QByteArray();
}

// Method to flush the C1 buffers
void C1Circ::flush(void)
{
    currentF3Data.fill(0);
    previousF3Data.fill(0);
    currentF3Errors.fill(0);
    previousF3Errors.fill(0);

    interleavedC1Data.fill(0);
    interleavedC1Errors.fill(0);

    outputC1Data.fill(0);
    outputC1Errors.fill(0);

    c1BufferLevel = 0;

    statistics.c1flushed++;
}

// Interleave current and previous F3 frame symbols and then invert parity symbols
void C1Circ::interleave(void)
{
    // Interleave the symbols
    for (qint32 byteC = 0; byteC < 32; byteC += 2) {
        interleavedC1Data[byteC] = currentF3Data[byteC];
        interleavedC1Data[byteC+1] = previousF3Data[byteC+1];

        interleavedC1Errors[byteC] = currentF3Errors[byteC];
        interleavedC1Errors[byteC+1] = previousF3Errors[byteC+1];
    }

    // Invert the Qm parity symbols
    interleavedC1Data[12] = static_cast<char>(static_cast<uchar>(interleavedC1Data[12]) ^ 0xFF);
    interleavedC1Data[13] = static_cast<char>(static_cast<uchar>(interleavedC1Data[13]) ^ 0xFF);
    interleavedC1Data[14] = static_cast<char>(static_cast<uchar>(interleavedC1Data[14]) ^ 0xFF);
    interleavedC1Data[15] = static_cast<char>(static_cast<uchar>(interleavedC1Data[15]) ^ 0xFF);

    // Invert the Pm parity symbols
    interleavedC1Data[28] = static_cast<char>(static_cast<uchar>(interleavedC1Data[28]) ^ 0xFF);
    interleavedC1Data[29] = static_cast<char>(static_cast<uchar>(interleavedC1Data[29]) ^ 0xFF);
    interleavedC1Data[30] = static_cast<char>(static_cast<uchar>(interleavedC1Data[30]) ^ 0xFF);
    interleavedC1Data[31] = static_cast<char>(static_cast<uchar>(interleavedC1Data[31]) ^ 0xFF);
}

// Perform a C1 level error check and correction
void C1Circ::errorCorrect(void)
{
    // Convert the data and errors into the form expected by the ezpwd library
    std::vector<uint8_t> data;
    std::vector<int> erasures;
    data.resize(32);

    for (qint32 byteC = 0; byteC < 32; byteC++) {
        data[static_cast<size_t>(byteC)] = static_cast<uchar>(interleavedC1Data[byteC]);
        if (interleavedC1Errors[byteC] == static_cast<char>(1)) erasures.push_back(byteC);
    }

    // Perform error check and correction
    int fixed = -1;
    if (erasures.size() > 4) erasures.clear();

    // Initialise the error corrector
    C1RS<255,255-4> rs; // Up to 251 symbols data load with 4 symbols parity RS(32,28)

    // Perform decode
    std::vector<int> position;
    fixed = rs.decode(data, erasures, &position);

    // Copy the result back to the output byte array (removing the parity symbols)
    for (qint32 byteC = 0; byteC < 28; byteC++) {
        outputC1Data[byteC] = static_cast<char>(data[static_cast<size_t>(byteC)]);
        if (fixed < 0) outputC1Errors[byteC] = 1; else outputC1Errors[byteC] = 0;
    }

    // Update the statistics
    if (fixed == 0) statistics.c1Passed++;
    if (fixed > 0)  statistics.c1Corrected++;
    if (fixed < 0)  statistics.c1Failed++;
}

