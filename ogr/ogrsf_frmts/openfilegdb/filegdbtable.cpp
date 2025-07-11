/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements reading of FileGDB tables
 * Author:   Even Rouault, <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "filegdbtable.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <limits>
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_time.h"
#include "cpl_vsi.h"
#include "filegdbtable_priv.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_geometry.h"
#include "ogrpgeogeometry.h"
#include "ogr_spatialref.h"

#define UUID_SIZE_IN_BYTES 16

#define IS_VALID_LAYER_GEOM_TYPE(byVal)                                        \
    ((byVal) <= FGTGT_POLYGON || (byVal) == FGTGT_MULTIPATCH)

/* Reserve one extra byte in case the last field is a string */
/*       or 2 for 2 ReadVarIntAndAddNoCheck() in a row */
/*       or 4 for SkipVarUInt() with nIter = 4 */
/*       or for 4 ReadVarUInt64NoCheck */
#define ZEROES_AFTER_END_OF_BUFFER 4

constexpr GUInt32 EXT_SHAPE_Z_FLAG = 0x80000000U;
constexpr GUInt32 EXT_SHAPE_M_FLAG = 0x40000000U;
constexpr GUInt32 EXT_SHAPE_CURVE_FLAG = 0x20000000U;

constexpr GUInt32 EXT_SHAPE_SEGMENT_ARC = 1;
constexpr GUInt32 EXT_SHAPE_SEGMENT_BEZIER = 4;
constexpr GUInt32 EXT_SHAPE_SEGMENT_ELLIPSE = 5;

namespace OpenFileGDB
{

FileGDBGeomField::~FileGDBGeomField() = default;

FileGDBRasterField::~FileGDBRasterField() = default;

/************************************************************************/
/*                           SanitizeScale()                            */
/************************************************************************/

static double SanitizeScale(double dfVal)
{
    if (dfVal == 0.0)
        return std::numeric_limits<double>::min();  // to prevent divide by zero
    return dfVal;
}

/************************************************************************/
/*                      FileGDBTablePrintError()                        */
/************************************************************************/

void FileGDBTablePrintError(const char *pszFile, int nLineNumber)
{
    CPLError(CE_Failure, CPLE_AppDefined, "Error occurred in %s at line %d",
             pszFile, nLineNumber);
}

/************************************************************************/
/*                            FileGDBTable()                            */
/************************************************************************/

FileGDBTable::FileGDBTable()
{
    memset(&m_sCurField, 0, sizeof(m_sCurField));
}

/************************************************************************/
/*                           ~FileGDBTable()                            */
/************************************************************************/

FileGDBTable::~FileGDBTable()
{
    Close();
}

/************************************************************************/
/*                                Close()                               */
/************************************************************************/

void FileGDBTable::Close()
{
    Sync();

    if (m_fpTable)
        VSIFCloseL(m_fpTable);
    m_fpTable = nullptr;

    if (m_fpTableX)
        VSIFCloseL(m_fpTableX);
    m_fpTableX = nullptr;
}

/************************************************************************/
/*                              GetFieldIdx()                           */
/************************************************************************/

int FileGDBTable::GetFieldIdx(const std::string &osName) const
{
    for (size_t i = 0; i < m_apoFields.size(); i++)
    {
        if (m_apoFields[i]->GetName() == osName)
            return static_cast<int>(i);
    }
    return -1;
}

/************************************************************************/
/*                          ReadVarUInt()                               */
/************************************************************************/

template <class OutType, class ControlType>
static int ReadVarUInt(GByte *&pabyIter, GByte *pabyEnd, OutType &nOutVal)
{
    const int errorRetValue = FALSE;
    if (!(ControlType::check_bounds))
    {
        /* nothing */
    }
    else if (ControlType::verbose_error)
    {
        returnErrorIf(pabyIter >= pabyEnd);
    }
    else
    {
        if (pabyIter >= pabyEnd)
            return FALSE;
    }
    OutType b = *pabyIter;
    if ((b & 0x80) == 0)
    {
        pabyIter++;
        nOutVal = b;
        return TRUE;
    }
    GByte *pabyLocalIter = pabyIter + 1;
    int nShift = 7;
    OutType nVal = (b & 0x7F);
    while (true)
    {
        if (!(ControlType::check_bounds))
        {
            /* nothing */
        }
        else if (ControlType::verbose_error)
        {
            returnErrorIf(pabyLocalIter >= pabyEnd);
        }
        else
        {
            if (pabyLocalIter >= pabyEnd)
                return FALSE;
        }
        b = *pabyLocalIter;
        pabyLocalIter++;
        nVal |= (b & 0x7F) << nShift;
        if ((b & 0x80) == 0)
        {
            pabyIter = pabyLocalIter;
            nOutVal = nVal;
            return TRUE;
        }
        nShift += 7;
        // To avoid undefined behavior later when doing << nShift
        if (nShift >= static_cast<int>(sizeof(OutType)) * 8)
        {
            pabyIter = pabyLocalIter;
            nOutVal = nVal;
            returnError();
        }
    }
}

struct ControlTypeVerboseErrorTrue
{
    // cppcheck-suppress unusedStructMember
    static const bool check_bounds = true;
    // cppcheck-suppress unusedStructMember
    static const bool verbose_error = true;
};

struct ControlTypeVerboseErrorFalse
{
    // cppcheck-suppress unusedStructMember
    static const bool check_bounds = true;
    // cppcheck-suppress unusedStructMember
    static const bool verbose_error = false;
};

struct ControlTypeNone
{
    // cppcheck-suppress unusedStructMember
    static const bool check_bounds = false;
    // cppcheck-suppress unusedStructMember
    static const bool verbose_error = false;
};

static int ReadVarUInt32(GByte *&pabyIter, GByte *pabyEnd, GUInt32 &nOutVal)
{
    return ReadVarUInt<GUInt32, ControlTypeVerboseErrorTrue>(pabyIter, pabyEnd,
                                                             nOutVal);
}

static void ReadVarUInt32NoCheck(GByte *&pabyIter, GUInt32 &nOutVal)
{
    GByte *pabyEnd = nullptr;
    ReadVarUInt<GUInt32, ControlTypeNone>(pabyIter, pabyEnd, nOutVal);
}

static int ReadVarUInt32Silent(GByte *&pabyIter, GByte *pabyEnd,
                               GUInt32 &nOutVal)
{
    return ReadVarUInt<GUInt32, ControlTypeVerboseErrorFalse>(pabyIter, pabyEnd,
                                                              nOutVal);
}

static void ReadVarUInt64NoCheck(GByte *&pabyIter, GUIntBig &nOutVal)
{
    GByte *pabyEnd = nullptr;
    ReadVarUInt<GUIntBig, ControlTypeNone>(pabyIter, pabyEnd, nOutVal);
}

/************************************************************************/
/*                      IsLikelyFeatureAtOffset()                       */
/************************************************************************/

int FileGDBTable::IsLikelyFeatureAtOffset(vsi_l_offset nOffset, GUInt32 *pnSize,
                                          int *pbDeletedRecord)
{
    VSIFSeekL(m_fpTable, nOffset, SEEK_SET);
    GByte abyBuffer[4];
    if (VSIFReadL(abyBuffer, 4, 1, m_fpTable) != 1)
        return FALSE;

    m_nRowBlobLength = GetUInt32(abyBuffer, 0);
    if (m_nRowBlobLength < static_cast<GUInt32>(m_nNullableFieldsSizeInBytes) ||
        m_nRowBlobLength > m_nFileSize - nOffset ||
        m_nRowBlobLength > INT_MAX - ZEROES_AFTER_END_OF_BUFFER ||
        m_nRowBlobLength > 10 * (m_nFileSize / m_nValidRecordCount))
    {
        /* Is it a deleted record ? */
        if ((m_nRowBlobLength >> (8 * sizeof(m_nRowBlobLength) - 1)) != 0 &&
            m_nRowBlobLength != 0x80000000U)
        {
            m_nRowBlobLength =
                static_cast<GUInt32>(-static_cast<int>(m_nRowBlobLength));
            if (m_nRowBlobLength <
                    static_cast<GUInt32>(m_nNullableFieldsSizeInBytes) ||
                m_nRowBlobLength > m_nFileSize - nOffset ||
                m_nRowBlobLength > INT_MAX - ZEROES_AFTER_END_OF_BUFFER ||
                m_nRowBlobLength > 10 * (m_nFileSize / m_nValidRecordCount))
                return FALSE;
            else
                *pbDeletedRecord = TRUE;
        }
        else
            return FALSE;
    }
    else
        *pbDeletedRecord = FALSE;

    m_nRowBufferMaxSize = std::max(m_nRowBlobLength, m_nRowBufferMaxSize);
    if (m_abyBuffer.size() < m_nRowBlobLength + ZEROES_AFTER_END_OF_BUFFER)
    {
        try
        {
            m_abyBuffer.resize(m_nRowBlobLength + ZEROES_AFTER_END_OF_BUFFER);
        }
        catch (const std::exception &e)
        {
            CPLError(CE_Failure, CPLE_OutOfMemory, "%s", e.what());
            return FALSE;
        }
    }
    if (m_nCountNullableFields > 0)
    {
        if (VSIFReadL(m_abyBuffer.data(), m_nNullableFieldsSizeInBytes, 1,
                      m_fpTable) != 1)
            return FALSE;
    }
    m_iAccNullable = 0;
    int bExactSizeKnown = TRUE;
    GUInt32 nRequiredLength = m_nNullableFieldsSizeInBytes;
    for (int i = 0; i < static_cast<int>(m_apoFields.size()); i++)
    {
        if (m_apoFields[i]->m_bNullable)
        {
            int bIsNull = TEST_BIT(m_abyBuffer.data(), m_iAccNullable);
            m_iAccNullable++;
            if (bIsNull)
                continue;
        }

        switch (m_apoFields[i]->m_eType)
        {
            case FGFT_UNDEFINED:
                CPLAssert(false);
                break;

            case FGFT_OBJECTID:
                break;

            case FGFT_STRING:
            case FGFT_XML:
            case FGFT_GEOMETRY:
            case FGFT_BINARY:
            {
                nRequiredLength += 1; /* varuint32 so at least one byte */
                bExactSizeKnown = FALSE;
                break;
            }

            case FGFT_RASTER:
            {
                const FileGDBRasterField *rasterField =
                    cpl::down_cast<const FileGDBRasterField *>(
                        m_apoFields[i].get());
                if (rasterField->GetRasterType() ==
                    FileGDBRasterField::Type::MANAGED)
                    nRequiredLength += sizeof(GInt32);
                else
                    nRequiredLength += 1; /* varuint32 so at least one byte */
                break;
            }

            case FGFT_INT16:
                nRequiredLength += sizeof(GInt16);
                break;
            case FGFT_INT32:
                nRequiredLength += sizeof(GInt32);
                break;
            case FGFT_FLOAT32:
                nRequiredLength += sizeof(float);
                break;
            case FGFT_FLOAT64:
                nRequiredLength += sizeof(double);
                break;
            case FGFT_DATETIME:
            case FGFT_DATE:
            case FGFT_TIME:
                nRequiredLength += sizeof(double);
                break;
            case FGFT_GUID:
            case FGFT_GLOBALID:
                nRequiredLength += UUID_SIZE_IN_BYTES;
                break;
            case FGFT_INT64:
                nRequiredLength += sizeof(int64_t);
                break;
            case FGFT_DATETIME_WITH_OFFSET:
                nRequiredLength += sizeof(double) + sizeof(int16_t);
                break;
        }
        if (m_nRowBlobLength < nRequiredLength)
            return FALSE;
    }
    if (!bExactSizeKnown)
    {
        if (VSIFReadL(m_abyBuffer.data() + m_nNullableFieldsSizeInBytes,
                      m_nRowBlobLength - m_nNullableFieldsSizeInBytes, 1,
                      m_fpTable) != 1)
            return FALSE;

        m_iAccNullable = 0;
        nRequiredLength = m_nNullableFieldsSizeInBytes;
        for (int i = 0; i < static_cast<int>(m_apoFields.size()); i++)
        {
            if (m_apoFields[i]->m_bNullable)
            {
                int bIsNull = TEST_BIT(m_abyBuffer.data(), m_iAccNullable);
                m_iAccNullable++;
                if (bIsNull)
                    continue;
            }

            switch (m_apoFields[i]->m_eType)
            {
                case FGFT_UNDEFINED:
                    CPLAssert(false);
                    break;

                case FGFT_OBJECTID:
                    break;

                case FGFT_STRING:
                case FGFT_XML:
                {
                    GByte *pabyIter = m_abyBuffer.data() + nRequiredLength;
                    GUInt32 nLength;
                    if (!ReadVarUInt32Silent(
                            pabyIter, m_abyBuffer.data() + m_nRowBlobLength,
                            nLength) ||
                        pabyIter - (m_abyBuffer.data() + nRequiredLength) > 5)
                        return FALSE;
                    nRequiredLength =
                        static_cast<GUInt32>(pabyIter - m_abyBuffer.data());
                    if (nLength > m_nRowBlobLength - nRequiredLength)
                        return FALSE;
                    for (GUInt32 j = 0; j < nLength; j++)
                    {
                        if (pabyIter[j] == 0)
                            return FALSE;
                    }
                    if (!CPLIsUTF8(reinterpret_cast<const char *>(pabyIter),
                                   nLength))
                        return FALSE;
                    nRequiredLength += nLength;
                    break;
                }

                case FGFT_GEOMETRY:
                case FGFT_BINARY:
                {
                    GByte *pabyIter = m_abyBuffer.data() + nRequiredLength;
                    GUInt32 nLength;
                    if (!ReadVarUInt32Silent(
                            pabyIter, m_abyBuffer.data() + m_nRowBlobLength,
                            nLength) ||
                        pabyIter - (m_abyBuffer.data() + nRequiredLength) > 5)
                        return FALSE;
                    nRequiredLength =
                        static_cast<GUInt32>(pabyIter - m_abyBuffer.data());
                    if (nLength > m_nRowBlobLength - nRequiredLength)
                        return FALSE;
                    nRequiredLength += nLength;
                    break;
                }

                case FGFT_RASTER:
                {
                    const FileGDBRasterField *rasterField =
                        cpl::down_cast<const FileGDBRasterField *>(
                            m_apoFields[i].get());
                    if (rasterField->GetRasterType() ==
                        FileGDBRasterField::Type::MANAGED)
                        nRequiredLength += sizeof(GInt32);
                    else
                    {
                        GByte *pabyIter = m_abyBuffer.data() + nRequiredLength;
                        GUInt32 nLength;
                        if (!ReadVarUInt32Silent(
                                pabyIter, m_abyBuffer.data() + m_nRowBlobLength,
                                nLength) ||
                            pabyIter - (m_abyBuffer.data() + nRequiredLength) >
                                5)
                            return FALSE;
                        nRequiredLength =
                            static_cast<GUInt32>(pabyIter - m_abyBuffer.data());
                        if (nLength > m_nRowBlobLength - nRequiredLength)
                            return FALSE;
                        nRequiredLength += nLength;
                    }
                    break;
                }

                case FGFT_INT16:
                    nRequiredLength += sizeof(GInt16);
                    break;
                case FGFT_INT32:
                    nRequiredLength += sizeof(GInt32);
                    break;
                case FGFT_FLOAT32:
                    nRequiredLength += sizeof(float);
                    break;
                case FGFT_FLOAT64:
                    nRequiredLength += sizeof(double);
                    break;
                case FGFT_DATETIME:
                case FGFT_DATE:
                case FGFT_TIME:
                    nRequiredLength += sizeof(double);
                    break;
                case FGFT_GUID:
                case FGFT_GLOBALID:
                    nRequiredLength += UUID_SIZE_IN_BYTES;
                    break;
                case FGFT_INT64:
                    nRequiredLength += sizeof(int64_t);
                    break;
                case FGFT_DATETIME_WITH_OFFSET:
                    nRequiredLength += sizeof(double) + sizeof(int16_t);
                    break;
            }
            if (nRequiredLength > m_nRowBlobLength)
                return FALSE;
        }
    }

    *pnSize = 4 + nRequiredLength;
    return nRequiredLength == m_nRowBlobLength;
}

/************************************************************************/
/*                      GuessFeatureLocations()                         */
/************************************************************************/

#define MARK_DELETED(x) ((x) | (static_cast<GUIntBig>(1) << 63))
#define IS_DELETED(x) (((x) & (static_cast<GUIntBig>(1) << 63)) != 0)
#define GET_OFFSET(x) ((x) & ~(static_cast<GUIntBig>(1) << 63))

bool FileGDBTable::GuessFeatureLocations()
{
    VSIFSeekL(m_fpTable, 0, SEEK_END);
    m_nFileSize = VSIFTellL(m_fpTable);

    int bReportDeletedFeatures = CPLTestBool(
        CPLGetConfigOption("OPENFILEGDB_REPORT_DELETED_FEATURES", "NO"));

    vsi_l_offset nOffset = 40 + m_nFieldDescLength;

    if (m_nOffsetFieldDesc != 40)
    {
        /* Check if there is a deleted field description at offset 40 */
        GByte abyBuffer[14];
        VSIFSeekL(m_fpTable, 40, SEEK_SET);
        if (VSIFReadL(abyBuffer, 14, 1, m_fpTable) != 1)
            return FALSE;
        int nSize = GetInt32(abyBuffer, 0);
        int nVersion = GetInt32(abyBuffer + 4, 0);
        if (nSize < 0 && nSize > -1024 * 1024 &&
            (nVersion == 3 || nVersion == 4) &&
            IS_VALID_LAYER_GEOM_TYPE(abyBuffer[8]) && abyBuffer[9] == 3 &&
            abyBuffer[10] == 0 && abyBuffer[11] == 0)
        {
            nOffset = 40 + (-nSize);
        }
        else
        {
            nOffset = 40;
        }
    }

    int64_t nInvalidRecords = 0;
    try
    {
        while (nOffset < m_nFileSize)
        {
            GUInt32 nSize;
            int bDeletedRecord;
            if (!IsLikelyFeatureAtOffset(nOffset, &nSize, &bDeletedRecord))
            {
                nOffset++;
            }
            else
            {
                /*CPLDebug("OpenFileGDB", "Feature found at offset %d (size = %d)",
                         nOffset, nSize);*/
                if (bDeletedRecord)
                {
                    if (bReportDeletedFeatures)
                    {
                        m_bHasDeletedFeaturesListed = TRUE;
                        m_anFeatureOffsets.push_back(MARK_DELETED(nOffset));
                    }
                    else
                    {
                        nInvalidRecords++;
                        m_anFeatureOffsets.push_back(0);
                    }
                }
                else
                    m_anFeatureOffsets.push_back(nOffset);
                nOffset += nSize;
            }
        }
    }
    catch (const std::bad_alloc &)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "Out of memory in FileGDBTable::GuessFeatureLocations()");
        return false;
    }
    m_nTotalRecordCount = static_cast<int64_t>(m_anFeatureOffsets.size());
    if (m_nTotalRecordCount - nInvalidRecords > m_nValidRecordCount)
    {
        if (!m_bHasDeletedFeaturesListed)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "More features found (%" PRId64
                     ") than declared number of valid "
                     "features ((%" PRId64 "). "
                     "So deleted features will likely be reported.",
                     m_nTotalRecordCount - nInvalidRecords,
                     m_nValidRecordCount);
        }
        m_nValidRecordCount = m_nTotalRecordCount - nInvalidRecords;
    }

    return m_nTotalRecordCount > 0;
}

/************************************************************************/
/*                           ReadTableXHeaderV3()                       */
/************************************************************************/

bool FileGDBTable::ReadTableXHeaderV3()
{
    const bool errorRetValue = false;
    GByte abyHeader[16];

    // Read .gdbtablx file header
    returnErrorIf(VSIFReadL(abyHeader, 16, 1, m_fpTableX) != 1);

    const int nGDBTablxVersion = GetUInt32(abyHeader, 0);
    if (nGDBTablxVersion != static_cast<int>(m_eGDBTableVersion))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 ".gdbtablx version is %d whereas it should be %d",
                 nGDBTablxVersion, static_cast<int>(m_eGDBTableVersion));
        return false;
    }

    m_n1024BlocksPresent = GetUInt32(abyHeader + 4, 0);

    m_nTotalRecordCount = GetInt32(abyHeader + 8, 0);
    if (m_n1024BlocksPresent == 0)
        returnErrorIf(m_nTotalRecordCount != 0);
    else
        returnErrorIf(m_nTotalRecordCount < 0);

    m_nTablxOffsetSize = GetUInt32(abyHeader + 12, 0);
    returnErrorIf(m_nTablxOffsetSize < 4 || m_nTablxOffsetSize > 6);

    m_nOffsetTableXTrailer =
        16 + m_nTablxOffsetSize * 1024 *
                 static_cast<vsi_l_offset>(m_n1024BlocksPresent);
    if (m_n1024BlocksPresent != 0)
    {
        GByte abyTrailer[16];

        VSIFSeekL(m_fpTableX, m_nOffsetTableXTrailer, SEEK_SET);
        returnErrorIf(VSIFReadL(abyTrailer, 16, 1, m_fpTableX) != 1);

        GUInt32 nBitmapInt32Words = GetUInt32(abyTrailer, 0);

        GUInt32 nBitsForBlockMap = GetUInt32(abyTrailer + 4, 0);
        returnErrorIf(nBitsForBlockMap > 1 + INT_MAX / 1024);

        GUInt32 n1024BlocksBis = GetUInt32(abyTrailer + 8, 0);
        returnErrorIf(n1024BlocksBis != m_n1024BlocksPresent);

        /* GUInt32 nLeadingNonZero32BitWords = GetUInt32(abyTrailer + 12, 0); */

        if (nBitmapInt32Words == 0)
        {
            returnErrorIf(nBitsForBlockMap != m_n1024BlocksPresent);
            /* returnErrorIf(nLeadingNonZero32BitWords != 0 ); */
        }
        else
        {
            returnErrorIf(static_cast<GUInt32>(m_nTotalRecordCount) >
                          nBitsForBlockMap * 1024);
#ifdef DEBUG_VERBOSE
            CPLDebug("OpenFileGDB", "%s .gdbtablx has block map array",
                     m_osFilename.c_str());
#endif

            // Allocate a bit mask array for blocks of 1024 features.
            uint32_t nSizeInBytes = BIT_ARRAY_SIZE_IN_BYTES(nBitsForBlockMap);
            try
            {
                m_abyTablXBlockMap.resize(nSizeInBytes);
            }
            catch (const std::exception &e)
            {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "Cannot allocate m_abyTablXBlockMap: %s", e.what());
                return false;
            }
            returnErrorIf(VSIFReadL(m_abyTablXBlockMap.data(), nSizeInBytes, 1,
                                    m_fpTableX) != 1);
            /* returnErrorIf(nMagic2 == 0 ); */

            // Check that the map is consistent with m_n1024BlocksPresent
            GUInt32 nCountBlocks = 0;
            for (GUInt32 i = 0; i < nBitsForBlockMap; i++)
                nCountBlocks += TEST_BIT(m_abyTablXBlockMap.data(), i) != 0;
            returnErrorIf(nCountBlocks != m_n1024BlocksPresent);
        }
    }
    return true;
}

/************************************************************************/
/*                           ReadTableXHeaderV4()                       */
/************************************************************************/

bool FileGDBTable::ReadTableXHeaderV4()
{
    const bool errorRetValue = false;
    GByte abyHeader[16];

    // Read .gdbtablx file header
    returnErrorIf(VSIFReadL(abyHeader, 16, 1, m_fpTableX) != 1);

    const int nGDBTablxVersion = GetUInt32(abyHeader, 0);
    if (nGDBTablxVersion != static_cast<int>(m_eGDBTableVersion))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 ".gdbtablx version is %d whereas it should be %d",
                 nGDBTablxVersion, static_cast<int>(m_eGDBTableVersion));
        return false;
    }

    m_n1024BlocksPresent = GetUInt64(abyHeader + 4, 0);

    m_nTablxOffsetSize = GetUInt32(abyHeader + 12, 0);
    returnErrorIf(m_nTablxOffsetSize < 4 || m_nTablxOffsetSize > 6);

    returnErrorIf(m_n1024BlocksPresent >
                  (std::numeric_limits<vsi_l_offset>::max() - 16) /
                      (m_nTablxOffsetSize * 1024));

    m_nOffsetTableXTrailer =
        16 + m_nTablxOffsetSize * 1024 *
                 static_cast<vsi_l_offset>(m_n1024BlocksPresent);
    if (m_n1024BlocksPresent != 0)
    {
        GByte abyTrailer[12];

        VSIFSeekL(m_fpTableX, m_nOffsetTableXTrailer, SEEK_SET);
        returnErrorIf(VSIFReadL(abyTrailer, 12, 1, m_fpTableX) != 1);

        m_nTotalRecordCount = GetUInt64(abyTrailer, 0);

        // Cf https://github.com/rouault/dump_gdbtable/wiki/FGDB-Spec#trailing-section-16-bytes--variable-number-
        // for all below magic numbers and byte sequences
        GUInt32 nSizeBitmapSection = GetUInt32(abyTrailer + 8, 0);
        if (nSizeBitmapSection == 0)
        {
            // no bitmap. Fine
        }
        else if (nSizeBitmapSection == 22 + 32768 + 52 &&
                 m_nTotalRecordCount <= 32768 * 1024 * 8)
        {
            try
            {
                std::vector<GByte> abyBitmapSection(nSizeBitmapSection);
                returnErrorIf(VSIFReadL(abyBitmapSection.data(),
                                        abyBitmapSection.size(), 1,
                                        m_fpTableX) != 1);
                if (memcmp(abyBitmapSection.data(), "\x01\x00\x01\x00\x00\x00",
                           6) == 0 &&
                    memcmp(abyBitmapSection.data() + 22 + 32768,
                           "\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
                           12) == 0)
                {
                    m_abyTablXBlockMap.insert(
                        m_abyTablXBlockMap.end(), abyBitmapSection.data() + 22,
                        abyBitmapSection.data() + 22 + 32768);
                }
                else
                {
                    m_bReliableObjectID = false;
                }
            }
            catch (const std::exception &e)
            {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "Cannot allocate m_abyTablXBlockMap: %s", e.what());
                return false;
            }
        }
        else
        {
            m_bReliableObjectID = false;
        }
        if (!m_bReliableObjectID)
        {
            m_nTotalRecordCount = 1024 * m_n1024BlocksPresent;
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Due to partial reverse engineering of the format, "
                     "ObjectIDs will not be accurate and attribute and spatial "
                     "indices cannot be used on %s",
                     m_osFilenameWithLayerName.c_str());
        }
    }
    return true;
}

/************************************************************************/
/*                                 Open()                               */
/************************************************************************/

bool FileGDBTable::Open(const char *pszFilename, bool bUpdate,
                        const char *pszLayerName)
{
    const bool errorRetValue = false;
    CPLAssert(m_fpTable == nullptr);

    m_bUpdate = bUpdate;

    m_osFilename = pszFilename;
    m_osFilenameWithLayerName = m_osFilename;
    if (pszLayerName)
        m_osFilenameWithLayerName += CPLSPrintf(" (layer %s)", pszLayerName);

    m_fpTable = VSIFOpenL(pszFilename, m_bUpdate ? "r+b" : "rb");
    if (m_fpTable == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "Cannot open %s: %s",
                 m_osFilenameWithLayerName.c_str(), VSIStrerror(errno));
        return false;
    }

    // Read .gdbtable file header
    GByte abyHeader[40];
    returnErrorIf(VSIFReadL(abyHeader, 40, 1, m_fpTable) != 1);

    int nGDBTableVersion = GetInt32(abyHeader, 0);
    if (nGDBTableVersion == 3)
    {
        m_eGDBTableVersion = GDBTableVersion::V3;
    }
    else if (nGDBTableVersion == 4)
    {
        m_eGDBTableVersion = GDBTableVersion::V4;
        if (m_bUpdate)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Version 4 of the FileGeodatabase format is not supported "
                     "for update.");
            return false;
        }
    }
    else
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Version %u of the FileGeodatabase format is not supported.",
                 nGDBTableVersion);
        return false;
    }

    if (m_eGDBTableVersion == GDBTableVersion::V3)
    {
        m_nValidRecordCount = GetInt32(abyHeader + 4, 0);
        returnErrorIf(m_nValidRecordCount < 0);
    }
    else
    {
        m_nValidRecordCount = GetInt64(abyHeader + 16, 0);
        returnErrorIf(m_nValidRecordCount < 0);
    }

    m_nHeaderBufferMaxSize = GetInt32(abyHeader + 8, 0);

    std::string osTableXName;
    if (m_bUpdate || (m_nValidRecordCount > 0 &&
                      !CPLTestBool(CPLGetConfigOption(
                          "OPENFILEGDB_IGNORE_GDBTABLX", "false"))))
    {
        osTableXName = CPLFormFilenameSafe(
            CPLGetPathSafe(pszFilename).c_str(),
            CPLGetBasenameSafe(pszFilename).c_str(), "gdbtablx");
        m_fpTableX = VSIFOpenL(osTableXName.c_str(), m_bUpdate ? "r+b" : "rb");
        if (m_fpTableX == nullptr)
        {
            if (m_bUpdate)
            {
                CPLError(CE_Failure, CPLE_OpenFailed, "Cannot open %s: %s",
                         osTableXName.c_str(), VSIStrerror(errno));
                return false;
            }
            const char *pszIgnoreGDBTablXAbsence = CPLGetConfigOption(
                "OPENFILEGDB_IGNORE_GDBTABLX_ABSENCE", nullptr);
            if (pszIgnoreGDBTablXAbsence == nullptr)
            {
                CPLError(
                    CE_Warning, CPLE_AppDefined,
                    "%s could not be found. "
                    "Trying to guess feature locations, but this might fail or "
                    "return incorrect results",
                    osTableXName.c_str());
            }
            else if (!CPLTestBool(pszIgnoreGDBTablXAbsence))
            {
                returnErrorIf(m_fpTableX == nullptr);
            }
        }
        else if (m_eGDBTableVersion == GDBTableVersion::V3 &&
                 !ReadTableXHeaderV3())
            return false;
        else if (m_eGDBTableVersion == GDBTableVersion::V4 &&
                 !ReadTableXHeaderV4())
            return false;
    }

    if (m_fpTableX != nullptr)
    {
        if (m_nValidRecordCount > m_nTotalRecordCount)
        {
            if (CPLTestBool(CPLGetConfigOption(
                    "OPENFILEGDB_USE_GDBTABLE_RECORD_COUNT", "false")))
            {
                /* Potentially unsafe. See #5842 */
                CPLDebug("OpenFileGDB",
                         "%s: nTotalRecordCount (was %" PRId64 ") forced to "
                         "nValidRecordCount=%" PRId64,
                         m_osFilenameWithLayerName.c_str(), m_nTotalRecordCount,
                         m_nValidRecordCount);
                m_nTotalRecordCount = m_nValidRecordCount;
            }
            else
            {
                /* By default err on the safe side */
                CPLError(
                    CE_Warning, CPLE_AppDefined,
                    "File %s declares %" PRId64
                    " valid records, but %s declares "
                    "only %" PRId64
                    " total records. Using that later value for safety "
                    "(this possibly ignoring features). "
                    "You can also try setting OPENFILEGDB_IGNORE_GDBTABLX=YES "
                    "to "
                    "completely ignore the .gdbtablx file (but possibly "
                    "retrieving "
                    "deleted features), or set "
                    "OPENFILEGDB_USE_GDBTABLE_RECORD_COUNT=YES "
                    "(but that setting can potentially cause crashes)",
                    m_osFilenameWithLayerName.c_str(), m_nValidRecordCount,
                    osTableXName.c_str(), m_nTotalRecordCount);
                m_nValidRecordCount = m_nTotalRecordCount;
            }
        }

#ifdef DEBUG_VERBOSE
        else if (m_nTotalRecordCount != m_nValidRecordCount)
        {
            CPLDebug("OpenFileGDB",
                     "%s: nTotalRecordCount=%" PRId64
                     " nValidRecordCount=%" PRId64,
                     pszFilename, m_nTotalRecordCount, m_nValidRecordCount);
        }
#endif
    }

    m_nOffsetFieldDesc = GetUInt64(abyHeader + 32, 0);

#ifdef DEBUG_VERBOSE
    if (m_nOffsetFieldDesc != 40)
    {
        CPLDebug("OpenFileGDB", "%s: nOffsetFieldDesc=" CPL_FRMT_GUIB,
                 pszFilename, m_nOffsetFieldDesc);
    }
#endif

    if (m_bUpdate)
    {
        VSIFSeekL(m_fpTable, 0, SEEK_END);
        m_nFileSize = VSIFTellL(m_fpTable);
    }

    // Skip to field description section
    VSIFSeekL(m_fpTable, m_nOffsetFieldDesc, SEEK_SET);
    returnErrorIf(VSIFReadL(abyHeader, 14, 1, m_fpTable) != 1);
    m_nFieldDescLength = GetUInt32(abyHeader, 0);

    const auto nSecondaryHeaderVersion = GetUInt32(abyHeader + 4, 0);
    // nSecondaryHeaderVersion == 6 is used in table arcgis_pro_32_types.gdb/a0000000b.gdbtable (big_int)
    // Not sure why...
    if (m_bUpdate && nSecondaryHeaderVersion != 4 &&
        nSecondaryHeaderVersion != 6)  // FileGDB v10
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Version %u of the secondary header of the FileGeodatabase "
                 "format is not supported for update.",
                 nSecondaryHeaderVersion);
        return false;
    }
    m_bIsV9 = (nSecondaryHeaderVersion == 3);

    returnErrorIf(m_nOffsetFieldDesc >
                  std::numeric_limits<GUIntBig>::max() - m_nFieldDescLength);

    returnErrorIf(m_nFieldDescLength > 10 * 1024 * 1024 ||
                  m_nFieldDescLength < 10);
    GByte byTableGeomType = abyHeader[8];
    if (IS_VALID_LAYER_GEOM_TYPE(byTableGeomType))
        m_eTableGeomType =
            static_cast<FileGDBTableGeometryType>(byTableGeomType);
    else
        CPLDebug("OpenFileGDB", "Unknown table geometry type: %d",
                 byTableGeomType);
    m_bStringsAreUTF8 = (abyHeader[9] & 0x1) != 0;
    const GByte byTableGeomTypeFlags = abyHeader[11];
    m_bGeomTypeHasM = (byTableGeomTypeFlags & (1 << 6)) != 0;
    m_bGeomTypeHasZ = (byTableGeomTypeFlags & (1 << 7)) != 0;

    GUInt16 iField, nFields;
    nFields = GetUInt16(abyHeader + 12, 0);

    /* No interest in guessing a trivial file */
    returnErrorIf(m_fpTableX == nullptr && nFields == 0);

    GUInt32 nRemaining = m_nFieldDescLength - 10;
    m_nRowBufferMaxSize = nRemaining;
    try
    {
        m_abyBuffer.resize(m_nRowBufferMaxSize + ZEROES_AFTER_END_OF_BUFFER);
    }
    catch (const std::exception &e)
    {
        CPLError(CE_Failure, CPLE_OutOfMemory, "%s", e.what());
        returnError();
    }
    returnErrorIf(VSIFReadL(m_abyBuffer.data(), nRemaining, 1, m_fpTable) != 1);

    GByte *pabyIter = m_abyBuffer.data();
    for (iField = 0; iField < nFields; iField++)
    {
        returnErrorIf(nRemaining < 1);
        GByte nCarCount = pabyIter[0];

        pabyIter++;
        nRemaining--;
        returnErrorIf(nCarCount > nRemaining / 2);
        std::string osName(ReadUTF16String(pabyIter, nCarCount));
        pabyIter += 2 * nCarCount;
        nRemaining -= 2 * nCarCount;

        returnErrorIf(nRemaining < 1);
        nCarCount = pabyIter[0];
        pabyIter++;
        nRemaining--;
        returnErrorIf(nCarCount > nRemaining / 2);
        std::string osAlias(ReadUTF16String(pabyIter, nCarCount));
        pabyIter += 2 * nCarCount;
        nRemaining -= 2 * nCarCount;

        returnErrorIf(nRemaining < 1);
        GByte byFieldType = pabyIter[0];
        pabyIter++;
        nRemaining--;

        if (byFieldType > FGFT_DATETIME_WITH_OFFSET)
        {
            CPLDebug("OpenFileGDB", "Unhandled field type : %d", byFieldType);
            returnError();
        }

        FileGDBFieldType eType = static_cast<FileGDBFieldType>(byFieldType);
        if (eType != FGFT_GEOMETRY && eType != FGFT_RASTER)
        {
            GByte flags = 0;
            int nMaxWidth = 0;
            GUInt32 defaultValueLength = 0;

            switch (eType)
            {
                case FGFT_STRING:
                {
                    returnErrorIf(nRemaining < 6);
                    nMaxWidth = GetInt32(pabyIter, 0);
                    returnErrorIf(nMaxWidth < 0);
                    flags = pabyIter[4];
                    pabyIter += 5;
                    nRemaining -= 5;
                    GByte *pabyIterBefore = pabyIter;
                    returnErrorIf(!ReadVarUInt32(
                        pabyIter, pabyIter + nRemaining, defaultValueLength));
                    nRemaining -=
                        static_cast<GUInt32>(pabyIter - pabyIterBefore);
                    break;
                }

                case FGFT_OBJECTID:
                case FGFT_BINARY:
                case FGFT_GUID:
                case FGFT_GLOBALID:
                case FGFT_XML:
                    returnErrorIf(nRemaining < 2);
                    flags = pabyIter[1];
                    pabyIter += 2;
                    nRemaining -= 2;
                    break;

                default:
                    returnErrorIf(nRemaining < 3);
                    flags = pabyIter[1];
                    defaultValueLength = pabyIter[2];
                    pabyIter += 3;
                    nRemaining -= 3;
                    break;
            }

            OGRField sDefault;
            OGR_RawField_SetUnset(&sDefault);
            if (flags & FileGDBField::MASK_EDITABLE)
            {
                /* Default value */
                /* Found on PreNIS.gdb/a0000000d.gdbtable */
                returnErrorIf(nRemaining < defaultValueLength);
                if (defaultValueLength)
                {
                    if (eType == FGFT_STRING)
                    {
                        if (m_bStringsAreUTF8)
                        {
                            sDefault.String = static_cast<char *>(
                                CPLMalloc(defaultValueLength + 1));
                            memcpy(sDefault.String, pabyIter,
                                   defaultValueLength);
                            sDefault.String[defaultValueLength] = 0;
                        }
                        else
                        {
                            m_osTempString = ReadUTF16String(
                                pabyIter, defaultValueLength / 2);
                            sDefault.String = CPLStrdup(m_osTempString.c_str());
                        }
                    }
                    else if (eType == FGFT_INT16 && defaultValueLength == 2)
                    {
                        sDefault.Integer = GetInt16(pabyIter, 0);
                        sDefault.Set.nMarker2 = 0;
                        sDefault.Set.nMarker3 = 0;
                    }
                    else if (eType == FGFT_INT32 && defaultValueLength == 4)
                    {
                        sDefault.Integer = GetInt32(pabyIter, 0);
                        sDefault.Set.nMarker2 = 0;
                        sDefault.Set.nMarker3 = 0;
                    }
                    else if (eType == FGFT_FLOAT32 && defaultValueLength == 4)
                    {
                        sDefault.Real = GetFloat32(pabyIter, 0);
                    }
                    else if (eType == FGFT_FLOAT64 && defaultValueLength == 8)
                    {
                        sDefault.Real = GetFloat64(pabyIter, 0);
                    }
                    else if ((eType == FGFT_DATETIME || eType == FGFT_DATE) &&
                             defaultValueLength == 8)
                    {
                        const double dfVal = GetFloat64(pabyIter, 0);
                        FileGDBDoubleDateToOGRDate(dfVal, true, &sDefault);
                    }
                    else if (eType == FGFT_TIME && defaultValueLength == 8)
                    {
                        const double dfVal = GetFloat64(pabyIter, 0);
                        FileGDBDoubleTimeToOGRTime(dfVal, &sDefault);
                    }
                    else if (eType == FGFT_INT64 && defaultValueLength == 8)
                    {
                        sDefault.Integer64 = GetInt64(pabyIter, 0);
                        sDefault.Set.nMarker3 = 0;
                    }
                    else if (eType == FGFT_DATETIME_WITH_OFFSET &&
                             defaultValueLength ==
                                 sizeof(double) + sizeof(int16_t))
                    {
                        const double dfVal = GetFloat64(pabyIter, 0);
                        const int16_t nUTCOffset =
                            GetInt16(pabyIter + sizeof(double), 0);
                        FileGDBDateTimeWithOffsetToOGRDate(dfVal, nUTCOffset,
                                                           &sDefault);
                    }
                }

                pabyIter += defaultValueLength;
                nRemaining -= defaultValueLength;
            }

            if (eType == FGFT_OBJECTID)
            {
                returnErrorIf(flags != FileGDBField::MASK_REQUIRED);
                returnErrorIf(m_iObjectIdField >= 0);
                m_iObjectIdField = static_cast<int>(m_apoFields.size());
            }

            auto poField = std::make_unique<FileGDBField>(this);
            poField->m_osName = std::move(osName);
            poField->m_osAlias = std::move(osAlias);
            poField->m_eType = eType;
            poField->m_bNullable = (flags & FileGDBField::MASK_NULLABLE) != 0;
            poField->m_bRequired = (flags & FileGDBField::MASK_REQUIRED) != 0;
            poField->m_bEditable = (flags & FileGDBField::MASK_EDITABLE) != 0;
            poField->m_nMaxWidth = nMaxWidth;
            poField->m_sDefault = sDefault;
            m_apoFields.emplace_back(std::move(poField));
        }
        else
        {

            FileGDBRasterField *poRasterField = nullptr;
            FileGDBGeomField *poField;
            if (eType == FGFT_GEOMETRY)
            {
                returnErrorIf(m_iGeomField >= 0);
                poField = new FileGDBGeomField(this);
            }
            else
            {
                poRasterField = new FileGDBRasterField(this);
                poField = poRasterField;
            }

            poField->m_osName = std::move(osName);
            poField->m_osAlias = std::move(osAlias);
            poField->m_eType = eType;
            if (eType == FGFT_GEOMETRY)
                m_iGeomField = static_cast<int>(m_apoFields.size());
            m_apoFields.emplace_back(
                std::unique_ptr<FileGDBGeomField>(poField));

            returnErrorIf(nRemaining < 2);
            GByte flags = pabyIter[1];
            poField->m_bNullable = (flags & 1) != 0;
            pabyIter += 2;
            nRemaining -= 2;

            if (eType == FGFT_RASTER)
            {
                returnErrorIf(nRemaining < 1);
                nCarCount = pabyIter[0];
                pabyIter++;
                nRemaining--;
                returnErrorIf(nRemaining <
                              static_cast<GUInt32>(2 * nCarCount + 1));
                poRasterField->m_osRasterColumnName =
                    ReadUTF16String(pabyIter, nCarCount);
                pabyIter += 2 * nCarCount;
                nRemaining -= 2 * nCarCount;
            }

            returnErrorIf(nRemaining < 2);
            GUInt16 nLengthWKT = GetUInt16(pabyIter, 0);
            pabyIter += sizeof(nLengthWKT);
            nRemaining -= sizeof(nLengthWKT);

            returnErrorIf(nRemaining < static_cast<GUInt32>(1 + nLengthWKT));
            poField->m_osWKT = ReadUTF16String(pabyIter, nLengthWKT / 2);
            pabyIter += nLengthWKT;
            nRemaining -= nLengthWKT;

            GByte abyGeomFlags = pabyIter[0];
            pabyIter++;
            nRemaining--;
            poField->m_bHasMOriginScaleTolerance = (abyGeomFlags & 2) != 0;
            poField->m_bHasZOriginScaleTolerance = (abyGeomFlags & 4) != 0;

            if (eType == FGFT_GEOMETRY || abyGeomFlags > 0)
            {
                returnErrorIf(nRemaining <
                              static_cast<GUInt32>(
                                  sizeof(double) *
                                  (4 + ((eType == FGFT_GEOMETRY) ? 4 : 0) +
                                   (poField->m_bHasMOriginScaleTolerance +
                                    poField->m_bHasZOriginScaleTolerance) *
                                       3)));

#define READ_DOUBLE(field)                                                     \
    do                                                                         \
    {                                                                          \
        field = GetFloat64(pabyIter, 0);                                       \
        pabyIter += sizeof(double);                                            \
        nRemaining -= sizeof(double);                                          \
    } while (false)

                READ_DOUBLE(poField->m_dfXOrigin);
                READ_DOUBLE(poField->m_dfYOrigin);
                READ_DOUBLE(poField->m_dfXYScale);
                returnErrorIf(poField->m_dfXYScale == 0);

                if (poField->m_bHasMOriginScaleTolerance)
                {
                    READ_DOUBLE(poField->m_dfMOrigin);
                    READ_DOUBLE(poField->m_dfMScale);
                }

                if (poField->m_bHasZOriginScaleTolerance)
                {
                    READ_DOUBLE(poField->m_dfZOrigin);
                    READ_DOUBLE(poField->m_dfZScale);
                }

                READ_DOUBLE(poField->m_dfXYTolerance);

                if (poField->m_bHasMOriginScaleTolerance)
                {
                    READ_DOUBLE(poField->m_dfMTolerance);
#ifdef DEBUG_VERBOSE
                    CPLDebug("OpenFileGDB",
                             "MOrigin = %g, MScale = %g, MTolerance = %g",
                             poField->m_dfMOrigin, poField->m_dfMScale,
                             poField->m_dfMTolerance);
#endif
                }

                if (poField->m_bHasZOriginScaleTolerance)
                {
                    READ_DOUBLE(poField->m_dfZTolerance);
                }
            }

            if (eType == FGFT_RASTER)
            {
                returnErrorIf(nRemaining < 1);
                if (*pabyIter == 0)
                    poRasterField->m_eRasterType =
                        FileGDBRasterField::Type::EXTERNAL;
                else if (*pabyIter == 1)
                    poRasterField->m_eRasterType =
                        FileGDBRasterField::Type::MANAGED;
                else if (*pabyIter == 2)
                    poRasterField->m_eRasterType =
                        FileGDBRasterField::Type::INLINE;
                else
                {
                    CPLError(CE_Warning, CPLE_NotSupported,
                             "Unknown raster field type %d", *pabyIter);
                }
                pabyIter += 1;
                nRemaining -= 1;
            }
            else
            {
                returnErrorIf(nRemaining < 4 * sizeof(double));
                m_nGeomFieldBBoxSubOffset =
                    static_cast<uint32_t>(pabyIter - m_abyBuffer.data()) + 14;
                READ_DOUBLE(poField->m_dfXMin);
                READ_DOUBLE(poField->m_dfYMin);
                READ_DOUBLE(poField->m_dfXMax);
                READ_DOUBLE(poField->m_dfYMax);

#ifdef PARANOID_CHECK
                const auto nCurrentPos = VSIFTellL(m_fpTable);
                VSIFSeekL(m_fpTable,
                          m_nOffsetFieldDesc + m_nGeomFieldBBoxSubOffset,
                          SEEK_SET);
                double dfXMinFromFile;
                VSIFReadL(&dfXMinFromFile, 1, sizeof(dfXMinFromFile),
                          m_fpTable);
                fprintf(stderr, "%f %f\n", dfXMinFromFile, /*ok*/
                        poField->m_dfXMin);
                double dfYMinFromFile;
                VSIFReadL(&dfYMinFromFile, 1, sizeof(dfYMinFromFile),
                          m_fpTable);
                fprintf(stderr, "%f %f\n", dfYMinFromFile, /*ok*/
                        poField->m_dfYMin);
                double dfXMaxFromFile;
                VSIFReadL(&dfXMaxFromFile, 1, sizeof(dfXMaxFromFile),
                          m_fpTable);
                fprintf(stderr, "%f %f\n", dfXMaxFromFile, /*ok*/
                        poField->m_dfXMax);
                double dfYMaxFromFile;
                VSIFReadL(&dfYMaxFromFile, 1, sizeof(dfYMaxFromFile),
                          m_fpTable);
                fprintf(stderr, "%f %f\n", dfYMaxFromFile, /*ok*/
                        poField->m_dfYMax);
                VSIFSeekL(m_fpTable, nCurrentPos, SEEK_SET);
#endif
                if (m_bGeomTypeHasZ)
                {
                    returnErrorIf(nRemaining < 2 * sizeof(double));
                    READ_DOUBLE(poField->m_dfZMin);
                    READ_DOUBLE(poField->m_dfZMax);
                }

                if (m_bGeomTypeHasM)
                {
                    returnErrorIf(nRemaining < 2 * sizeof(double));
                    READ_DOUBLE(poField->m_dfMMin);
                    READ_DOUBLE(poField->m_dfMMax);
                }

                returnErrorIf(nRemaining < 5);
                // Skip byte at zero
                pabyIter += 1;
                nRemaining -= 1;

                GUInt32 nGridSizeCount = GetUInt32(pabyIter, 0);
                pabyIter += sizeof(nGridSizeCount);
                nRemaining -= sizeof(nGridSizeCount);
                returnErrorIf(nGridSizeCount == 0 || nGridSizeCount > 3);
                returnErrorIf(nRemaining < nGridSizeCount * sizeof(double));
                m_nGeomFieldSpatialIndexGridResSubOffset =
                    static_cast<uint32_t>(pabyIter - m_abyBuffer.data()) + 14;
                for (GUInt32 i = 0; i < nGridSizeCount; i++)
                {
                    double dfGridResolution;
                    READ_DOUBLE(dfGridResolution);
                    m_adfSpatialIndexGridResolution.push_back(dfGridResolution);
                }
                poField->m_adfSpatialIndexGridResolution =
                    m_adfSpatialIndexGridResolution;
            }
        }

        m_nCountNullableFields +=
            static_cast<int>(m_apoFields.back()->m_bNullable);
    }
    m_nNullableFieldsSizeInBytes =
        BIT_ARRAY_SIZE_IN_BYTES(m_nCountNullableFields);

#ifdef DEBUG_VERBOSE
    if (nRemaining > 0)
    {
        CPLDebug("OpenFileGDB",
                 "%u remaining (ignored) bytes in field header section",
                 nRemaining);
    }
#endif

    if (m_nValidRecordCount > 0 && m_fpTableX == nullptr)
        return GuessFeatureLocations();

    return true;
}

/************************************************************************/
/*                          SkipVarUInt()                               */
/************************************************************************/

/* Bound check only valid if nIter <= 4 */
static int SkipVarUInt(GByte *&pabyIter, GByte *pabyEnd, int nIter = 1)
{
    const int errorRetValue = FALSE;
    GByte *pabyLocalIter = pabyIter;
    returnErrorIf(pabyLocalIter /*+ nIter - 1*/ >= pabyEnd);
    while (nIter-- > 0)
    {
        while (true)
        {
            GByte b = *pabyLocalIter;
            pabyLocalIter++;
            if ((b & 0x80) == 0)
                break;
        }
    }
    pabyIter = pabyLocalIter;
    return TRUE;
}

/************************************************************************/
/*                      ReadVarIntAndAddNoCheck()                       */
/************************************************************************/

CPL_NOSANITIZE_UNSIGNED_INT_OVERFLOW
static void ReadVarIntAndAddNoCheck(GByte *&pabyIter, GIntBig &nOutVal)
{
    GUInt32 b;

    b = *pabyIter;
    GUIntBig nVal = (b & 0x3F);
    bool bNegative = (b & 0x40) != 0;
    if ((b & 0x80) == 0)
    {
        pabyIter++;
        if (bNegative)
            nOutVal -= nVal;
        else
            nOutVal += nVal;
        return;
    }

    GByte *pabyLocalIter = pabyIter + 1;
    int nShift = 6;
    while (true)
    {
        GUIntBig b64 = *pabyLocalIter;
        pabyLocalIter++;
        nVal |= (b64 & 0x7F) << nShift;
        if ((b64 & 0x80) == 0)
        {
            pabyIter = pabyLocalIter;
            if (bNegative)
                nOutVal -= nVal;
            else
                nOutVal += nVal;
            return;
        }
        nShift += 7;
        // To avoid undefined behavior later when doing << nShift
        if (nShift >= static_cast<int>(sizeof(GIntBig)) * 8)
        {
            pabyIter = pabyLocalIter;
            nOutVal = nVal;
            return;
        }
    }
}

/************************************************************************/
/*                       GetOffsetInTableForRow()                       */
/************************************************************************/

vsi_l_offset
FileGDBTable::GetOffsetInTableForRow(int64_t iRow,
                                     vsi_l_offset *pnOffsetInTableX)
{
    const int errorRetValue = 0;
    if (pnOffsetInTableX)
        *pnOffsetInTableX = 0;
    returnErrorIf(iRow < 0 || iRow >= m_nTotalRecordCount);

    m_bIsDeleted = false;
    if (m_fpTableX == nullptr)
    {
        m_bIsDeleted =
            IS_DELETED(m_anFeatureOffsets[static_cast<size_t>(iRow)]);
        return GET_OFFSET(m_anFeatureOffsets[static_cast<size_t>(iRow)]);
    }

    vsi_l_offset nOffsetInTableX;
    if (!m_abyTablXBlockMap.empty())
    {
        GUInt32 nCountBlocksBefore = 0;
        const int iBlock = static_cast<int>(iRow / 1024);

        // Check if the block is not empty
        if (TEST_BIT(m_abyTablXBlockMap.data(), iBlock) == 0)
            return 0;

        // In case of sequential reading, optimization to avoid recomputing
        // the number of blocks since the beginning of the map
        if (iBlock >= m_nCountBlocksBeforeIBlockIdx)
        {
            nCountBlocksBefore = m_nCountBlocksBeforeIBlockValue;
            for (int i = m_nCountBlocksBeforeIBlockIdx; i < iBlock; i++)
                nCountBlocksBefore +=
                    TEST_BIT(m_abyTablXBlockMap.data(), i) != 0;
        }
        else
        {
            nCountBlocksBefore = 0;
            for (int i = 0; i < iBlock; i++)
                nCountBlocksBefore +=
                    TEST_BIT(m_abyTablXBlockMap.data(), i) != 0;
        }
        m_nCountBlocksBeforeIBlockIdx = iBlock;
        m_nCountBlocksBeforeIBlockValue = nCountBlocksBefore;
        const int64_t iCorrectedRow =
            static_cast<int64_t>(nCountBlocksBefore) * 1024 + (iRow % 1024);
        nOffsetInTableX =
            16 + static_cast<vsi_l_offset>(m_nTablxOffsetSize) * iCorrectedRow;
    }
    else
    {
        nOffsetInTableX =
            16 + static_cast<vsi_l_offset>(m_nTablxOffsetSize) * iRow;
    }

    if (pnOffsetInTableX)
        *pnOffsetInTableX = nOffsetInTableX;
    VSIFSeekL(m_fpTableX, nOffsetInTableX, SEEK_SET);

    GByte abyBuffer[6];
    m_bError = VSIFReadL(abyBuffer, m_nTablxOffsetSize, 1, m_fpTableX) != 1;
    returnErrorIf(m_bError);

    const vsi_l_offset nOffset = ReadFeatureOffset(abyBuffer);

#ifdef DEBUG_VERBOSE
    const auto nOffsetHeaderEnd = m_nOffsetFieldDesc + m_nFieldDescLength;
    if (iRow == 0 && nOffset != 0 && nOffset != nOffsetHeaderEnd &&
        nOffset != nOffsetHeaderEnd + 4)
        CPLDebug("OpenFileGDB",
                 "%s: first feature offset = " CPL_FRMT_GUIB
                 ". Expected " CPL_FRMT_GUIB,
                 m_osFilename.c_str(), nOffset, nOffsetHeaderEnd);
#endif

    return nOffset;
}

/************************************************************************/
/*                        ReadFeatureOffset()                           */
/************************************************************************/

uint64_t FileGDBTable::ReadFeatureOffset(const GByte *pabyBuffer)
{
    uint64_t nOffset = 0;
    memcpy(&nOffset, pabyBuffer, m_nTablxOffsetSize);
    CPL_LSBPTR64(&nOffset);
    return nOffset;
}

/************************************************************************/
/*                      GetAndSelectNextNonEmptyRow()                   */
/************************************************************************/

int64_t FileGDBTable::GetAndSelectNextNonEmptyRow(int64_t iRow)
{
    const int64_t errorRetValue = -1;
    returnErrorAndCleanupIf(iRow < 0 || iRow >= m_nTotalRecordCount,
                            m_nCurRow = -1);

    while (iRow < m_nTotalRecordCount)
    {
        if (!m_abyTablXBlockMap.empty() && (iRow % 1024) == 0)
        {
            int iBlock = static_cast<int>(iRow / 1024);
            if (TEST_BIT(m_abyTablXBlockMap.data(), iBlock) == 0)
            {
                int nBlocks =
                    static_cast<int>(DIV_ROUND_UP(m_nTotalRecordCount, 1024));
                do
                {
                    iBlock++;
                } while (iBlock < nBlocks &&
                         TEST_BIT(m_abyTablXBlockMap.data(), iBlock) == 0);

                iRow = static_cast<int64_t>(iBlock) * 1024;
                if (iRow >= m_nTotalRecordCount)
                    return -1;
            }
        }

        if (SelectRow(iRow))
            return iRow;
        if (HasGotError())
            return -1;
        iRow++;
    }

    return -1;
}

/************************************************************************/
/*                            SelectRow()                               */
/************************************************************************/

bool FileGDBTable::SelectRow(int64_t iRow)
{
    const int errorRetValue = FALSE;
    returnErrorAndCleanupIf(iRow < 0 || iRow >= m_nTotalRecordCount,
                            m_nCurRow = -1);

    if (m_nCurRow != iRow)
    {
        vsi_l_offset nOffsetTable = GetOffsetInTableForRow(iRow);
        if (nOffsetTable == 0)
        {
            m_nCurRow = -1;
            return FALSE;
        }

        VSIFSeekL(m_fpTable, nOffsetTable, SEEK_SET);
        GByte abyBuffer[4];
        returnErrorAndCleanupIf(VSIFReadL(abyBuffer, 4, 1, m_fpTable) != 1,
                                m_nCurRow = -1);

        m_nRowBlobLength = GetUInt32(abyBuffer, 0);
        if (m_bIsDeleted)
        {
            m_nRowBlobLength =
                static_cast<GUInt32>(-static_cast<int>(m_nRowBlobLength));
        }

        if (m_nRowBlobLength > 0)
        {
            /* CPLDebug("OpenFileGDB", "nRowBlobLength = %u", nRowBlobLength);
             */
            returnErrorAndCleanupIf(
                m_nRowBlobLength <
                        static_cast<GUInt32>(m_nNullableFieldsSizeInBytes) ||
                    m_nRowBlobLength > INT_MAX - ZEROES_AFTER_END_OF_BUFFER,
                m_nCurRow = -1);

            if (m_nRowBlobLength > m_nHeaderBufferMaxSize)
            {
                if (CPLTestBool(CPLGetConfigOption(
                        "OGR_OPENFILEGDB_ERROR_ON_INCONSISTENT_BUFFER_MAX_SIZE",
                        "NO")))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Invalid row length (%u) on feature %" PRId64
                             " compared "
                             "to the maximum size in the header (%u)",
                             m_nRowBlobLength, iRow + 1,
                             m_nHeaderBufferMaxSize);
                    m_nCurRow = -1;
                    return errorRetValue;
                }
                else
                {
                    // Versions of the driver before commit
                    // fdf39012788b1110b3bf0ae6b8422a528f0ae8b6 didn't
                    // properly update the m_nHeaderBufferMaxSize field
                    // when updating an existing feature when the new version
                    // takes more space than the previous version.
                    // OpenFileGDB doesn't care but Esri software (FileGDB SDK
                    // or ArcMap/ArcGis) do, leading to issues such as
                    // https://github.com/qgis/QGIS/issues/57536

                    CPLDebug("OpenFileGDB",
                             "Invalid row length (%u) on feature %" PRId64
                             " compared "
                             "to the maximum size in the header (%u)",
                             m_nRowBlobLength, iRow + 1,
                             m_nHeaderBufferMaxSize);

                    if (m_bUpdate)
                    {
                        if (!m_bHasWarnedAboutHeaderRepair)
                        {
                            m_bHasWarnedAboutHeaderRepair = true;
                            CPLError(CE_Warning, CPLE_AppDefined,
                                     "A corruption in the header of %s has "
                                     "been detected. It is going to be "
                                     "repaired to be properly read by other "
                                     "software.",
                                     m_osFilename.c_str());

                            m_bDirtyHeader = true;

                            // Invalidate existing indices, as the corrupted
                            // m_nHeaderBufferMaxSize value may have cause
                            // Esri software to generate corrupted indices.
                            m_bDirtyIndices = true;

                            // Compute file size
                            VSIFSeekL(m_fpTable, 0, SEEK_END);
                            m_nFileSize = VSIFTellL(m_fpTable);
                            VSIFSeekL(m_fpTable, nOffsetTable + 4, SEEK_SET);
                        }
                    }
                    else
                    {
                        if (!m_bHasWarnedAboutHeaderRepair)
                        {
                            m_bHasWarnedAboutHeaderRepair = true;
                            CPLError(CE_Warning, CPLE_AppDefined,
                                     "A corruption in the header of %s has "
                                     "been detected. It would need to be "
                                     "repaired to be properly read by other "
                                     "software, either by using ogr2ogr to "
                                     "generate a new dataset, or by opening "
                                     "this dataset in update mode and reading "
                                     "all its records.",
                                     m_osFilename.c_str());
                        }
                    }

                    m_nHeaderBufferMaxSize = m_nRowBlobLength;
                }
            }

            if (m_nRowBlobLength > m_nRowBufferMaxSize)
            {
                /* For suspicious row blob length, check if we don't go beyond
                 * file size */
                if (m_nRowBlobLength > 100 * 1024 * 1024)
                {
                    if (m_nFileSize == 0)
                    {
                        VSIFSeekL(m_fpTable, 0, SEEK_END);
                        m_nFileSize = VSIFTellL(m_fpTable);
                        VSIFSeekL(m_fpTable, nOffsetTable + 4, SEEK_SET);
                    }
                    if (nOffsetTable + 4 + m_nRowBlobLength > m_nFileSize)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Invalid row length (%u) on feature %" PRId64,
                                 m_nRowBlobLength, iRow + 1);
                        m_nCurRow = -1;
                        return errorRetValue;
                    }
                }
                m_nRowBufferMaxSize = m_nRowBlobLength;
            }

            if (m_abyBuffer.size() <
                m_nRowBlobLength + ZEROES_AFTER_END_OF_BUFFER)
            {
                try
                {
                    m_abyBuffer.resize(m_nRowBlobLength +
                                       ZEROES_AFTER_END_OF_BUFFER);
                }
                catch (const std::exception &e)
                {
                    CPLError(CE_Failure, CPLE_OutOfMemory, "%s", e.what());
                    returnErrorAndCleanupIf(true, m_nCurRow = -1);
                }
            }

            returnErrorAndCleanupIf(VSIFReadL(m_abyBuffer.data(),
                                              m_nRowBlobLength, 1,
                                              m_fpTable) != 1,
                                    m_nCurRow = -1);
            /* Protection for 4 ReadVarUInt64NoCheck */
            CPL_STATIC_ASSERT(ZEROES_AFTER_END_OF_BUFFER == 4);
            m_abyBuffer[m_nRowBlobLength] = 0;
            m_abyBuffer[m_nRowBlobLength + 1] = 0;
            m_abyBuffer[m_nRowBlobLength + 2] = 0;
            m_abyBuffer[m_nRowBlobLength + 3] = 0;
        }

        m_nCurRow = iRow;
        m_nLastCol = -1;
        m_pabyIterVals = m_abyBuffer.data() + m_nNullableFieldsSizeInBytes;
        m_iAccNullable = 0;
        m_bError = FALSE;
        m_nChSaved = -1;
    }

    return TRUE;
}

/************************************************************************/
/*                      FileGDBDoubleDateToOGRDate()                    */
/************************************************************************/

int FileGDBDoubleDateToOGRDate(double dfVal, bool bHighPrecision,
                               OGRField *psField)
{
    // 25569: Number of days between 1899/12/30 00:00:00 and 1970/01/01 00:00:00
    double dfSeconds = (dfVal - 25569.0) * 3600.0 * 24.0;
    if (std::isnan(dfSeconds) ||
        dfSeconds <
            static_cast<double>(std::numeric_limits<GIntBig>::min()) + 1000 ||
        dfSeconds >
            static_cast<double>(std::numeric_limits<GIntBig>::max()) - 1000)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "FileGDBDoubleDateToOGRDate: Invalid days: %lf", dfVal);
        dfSeconds = 0.0;
    }
    if (!bHighPrecision)
        dfSeconds = std::floor(dfSeconds + 0.5);
    else if (fmod(dfSeconds, 1.0) > 1 - 1e-4)
        dfSeconds = std::floor(dfSeconds + 0.5);

    struct tm brokendowntime;
    CPLUnixTimeToYMDHMS(static_cast<GIntBig>(dfSeconds), &brokendowntime);

    psField->Date.Year = static_cast<GInt16>(brokendowntime.tm_year + 1900);
    psField->Date.Month = static_cast<GByte>(brokendowntime.tm_mon + 1);
    psField->Date.Day = static_cast<GByte>(brokendowntime.tm_mday);
    psField->Date.Hour = static_cast<GByte>(brokendowntime.tm_hour);
    psField->Date.Minute = static_cast<GByte>(brokendowntime.tm_min);
    double dfSec = brokendowntime.tm_sec;
    if (bHighPrecision)
    {
        dfSec += fmod(dfSeconds, 1.0);
    }
    psField->Date.Second = static_cast<float>(dfSec);
    psField->Date.TZFlag = 0;
    psField->Date.Reserved = 0;

    return TRUE;
}

/************************************************************************/
/*                      FileGDBDoubleTimeToOGRTime()                    */
/************************************************************************/

int FileGDBDoubleTimeToOGRTime(double dfVal, OGRField *psField)
{
    double dfSeconds = dfVal * 3600.0 * 24.0;
    if (std::isnan(dfSeconds) || dfSeconds < 0 || dfSeconds > 86400)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "FileGDBDoubleTimeToOGRTime: Invalid time: %lf", dfVal);
        dfSeconds = 0.0;
    }

    psField->Date.Year = 0;
    psField->Date.Month = 0;
    psField->Date.Day = 0;
    psField->Date.Hour = static_cast<GByte>(dfSeconds / 3600);
    psField->Date.Minute =
        static_cast<GByte>((static_cast<int>(dfSeconds) % 3600) / 60);
    psField->Date.Second = static_cast<float>(fmod(dfSeconds, 60));
    psField->Date.TZFlag = 0;
    psField->Date.Reserved = 0;

    return TRUE;
}

/************************************************************************/
/*                  FileGDBDateTimeWithOffsetToOGRDate()                */
/************************************************************************/

int FileGDBDateTimeWithOffsetToOGRDate(double dfVal, int16_t nUTCOffset,
                                       OGRField *psField)
{
    int ret = FileGDBDoubleDateToOGRDate(dfVal, true, psField);
    if (nUTCOffset >= -14 * 60 && nUTCOffset <= 14 * 60)
    {
        psField->Date.TZFlag = static_cast<GByte>(100 + nUTCOffset / 15);
    }
    else
        ret = FALSE;
    return ret;
}

/************************************************************************/
/*                         GetAllFieldValues()                          */
/************************************************************************/

std::vector<OGRField> FileGDBTable::GetAllFieldValues()
{
    std::vector<OGRField> asFields(m_apoFields.size(),
                                   FileGDBField::UNSET_FIELD);
    for (int i = 0; i < static_cast<int>(m_apoFields.size()); ++i)
    {
        const OGRField *psField = GetFieldValue(i);
        if (psField && !OGR_RawField_IsNull(psField) &&
            !OGR_RawField_IsUnset(psField) &&
            (m_apoFields[i]->GetType() == FGFT_STRING ||
             m_apoFields[i]->GetType() == FGFT_XML ||
             m_apoFields[i]->GetType() == FGFT_GLOBALID ||
             m_apoFields[i]->GetType() == FGFT_GUID))
        {
            asFields[i].String = CPLStrdup(psField->String);
        }
        else if (psField && !OGR_RawField_IsNull(psField) &&
                 !OGR_RawField_IsUnset(psField) &&
                 (m_apoFields[i]->GetType() == FGFT_BINARY ||
                  m_apoFields[i]->GetType() == FGFT_GEOMETRY))
        {
            asFields[i].Binary.paData =
                static_cast<GByte *>(CPLMalloc(psField->Binary.nCount));
            asFields[i].Binary.nCount = psField->Binary.nCount;
            memcpy(asFields[i].Binary.paData, psField->Binary.paData,
                   asFields[i].Binary.nCount);
        }
        else if (psField && !(m_apoFields[i]->GetType() == FGFT_RASTER))
        {
            asFields[i] = *psField;
        }
    }
    return asFields;
}

/************************************************************************/
/*                        FreeAllFieldValues()                          */
/************************************************************************/

void FileGDBTable::FreeAllFieldValues(std::vector<OGRField> &asFields)
{
    for (int i = 0; i < static_cast<int>(m_apoFields.size()); ++i)
    {
        if (!OGR_RawField_IsNull(&asFields[i]) &&
            !OGR_RawField_IsUnset(&asFields[i]) &&
            (m_apoFields[i]->GetType() == FGFT_STRING ||
             m_apoFields[i]->GetType() == FGFT_XML ||
             m_apoFields[i]->GetType() == FGFT_GLOBALID ||
             m_apoFields[i]->GetType() == FGFT_GUID))
        {
            CPLFree(asFields[i].String);
            asFields[i].String = nullptr;
        }
        else if (!OGR_RawField_IsNull(&asFields[i]) &&
                 !OGR_RawField_IsUnset(&asFields[i]) &&
                 (m_apoFields[i]->GetType() == FGFT_BINARY ||
                  m_apoFields[i]->GetType() == FGFT_GEOMETRY))
        {
            CPLFree(asFields[i].Binary.paData);
            asFields[i].Binary.paData = nullptr;
        }
    }
}

/************************************************************************/
/*                          GetFieldValue()                             */
/************************************************************************/

const OGRField *FileGDBTable::GetFieldValue(int iCol)
{
    OGRField *errorRetValue = nullptr;

    returnErrorIf(m_nCurRow < 0);
    returnErrorIf(static_cast<GUInt32>(iCol) >= m_apoFields.size());
    returnErrorIf(m_bError);

    GByte *pabyEnd = m_abyBuffer.data() + m_nRowBlobLength;

    /* In case a string was previously read */
    if (m_nChSaved >= 0)
    {
        *m_pabyIterVals = static_cast<GByte>(m_nChSaved);
        m_nChSaved = -1;
    }

    if (iCol <= m_nLastCol)
    {
        m_nLastCol = -1;
        m_pabyIterVals = m_abyBuffer.data() + m_nNullableFieldsSizeInBytes;
        m_iAccNullable = 0;
    }

    // Skip previous fields
    for (int j = m_nLastCol + 1; j < iCol; j++)
    {
        if (m_apoFields[j]->m_bNullable)
        {
            int bIsNull = TEST_BIT(m_abyBuffer.data(), m_iAccNullable);
            m_iAccNullable++;
            if (bIsNull)
                continue;
        }

        GUInt32 nLength = 0;
        CPL_IGNORE_RET_VAL(nLength);
        switch (m_apoFields[j]->m_eType)
        {
            case FGFT_UNDEFINED:
                CPLAssert(false);
                break;

            case FGFT_OBJECTID:
                break;

            case FGFT_STRING:
            case FGFT_XML:
            case FGFT_GEOMETRY:
            case FGFT_BINARY:
            {
                if (!ReadVarUInt32(m_pabyIterVals, pabyEnd, nLength))
                {
                    m_bError = TRUE;
                    returnError();
                }
                break;
            }

            case FGFT_RASTER:
            {
                const FileGDBRasterField *rasterField =
                    cpl::down_cast<const FileGDBRasterField *>(
                        m_apoFields[j].get());
                if (rasterField->GetRasterType() ==
                    FileGDBRasterField::Type::MANAGED)
                    nLength = sizeof(GInt32);
                else
                {
                    if (!ReadVarUInt32(m_pabyIterVals, pabyEnd, nLength))
                    {
                        m_bError = TRUE;
                        returnError();
                    }
                }
                break;
            }

            case FGFT_INT16:
                nLength = sizeof(GInt16);
                break;
            case FGFT_INT32:
                nLength = sizeof(GInt32);
                break;
            case FGFT_FLOAT32:
                nLength = sizeof(float);
                break;
            case FGFT_FLOAT64:
                nLength = sizeof(double);
                break;
            case FGFT_DATETIME:
            case FGFT_DATE:
            case FGFT_TIME:
                nLength = sizeof(double);
                break;
            case FGFT_GUID:
            case FGFT_GLOBALID:
                nLength = UUID_SIZE_IN_BYTES;
                break;
            case FGFT_INT64:
                nLength = sizeof(int64_t);
                break;
            case FGFT_DATETIME_WITH_OFFSET:
                nLength += sizeof(double) + sizeof(int16_t);
                break;
        }

        if (nLength > static_cast<GUInt32>(pabyEnd - m_pabyIterVals))
        {
            m_bError = TRUE;
            returnError();
        }
        m_pabyIterVals += nLength;
    }

    m_nLastCol = iCol;

    if (m_apoFields[iCol]->m_bNullable)
    {
        int bIsNull = TEST_BIT(m_abyBuffer.data(), m_iAccNullable);
        m_iAccNullable++;
        if (bIsNull)
        {
            return nullptr;
        }
    }

    switch (m_apoFields[iCol]->m_eType)
    {
        case FGFT_UNDEFINED:
            CPLAssert(false);
            break;

        case FGFT_OBJECTID:
            return nullptr;

        case FGFT_STRING:
        case FGFT_XML:
        {
            GUInt32 nLength;
            if (!ReadVarUInt32(m_pabyIterVals, pabyEnd, nLength))
            {
                m_bError = TRUE;
                returnError();
            }
            if (nLength > static_cast<GUInt32>(pabyEnd - m_pabyIterVals))
            {
                m_bError = TRUE;
                returnError();
            }

            if (m_bStringsAreUTF8 || m_apoFields[iCol]->m_eType != FGFT_STRING)
            {
                /* eCurFieldType = OFTString; */
                m_sCurField.String = reinterpret_cast<char *>(m_pabyIterVals);
                m_pabyIterVals += nLength;

                /* This is a trick to avoid a alloc()+copy(). We null-terminate
                 */
                /* after the string, and save the pointer and value to restore
                 */
                m_nChSaved = *m_pabyIterVals;
                *m_pabyIterVals = '\0';
            }
            else
            {
                m_osTempString = ReadUTF16String(m_pabyIterVals, nLength / 2);
                m_sCurField.String = &m_osTempString[0];
                m_pabyIterVals += nLength;
            }

            /* CPLDebug("OpenFileGDB", "Field %d, row %d: %s", iCol, nCurRow,
             * sCurField.String); */

            break;
        }

        case FGFT_INT16:
        {
            if (m_pabyIterVals + sizeof(GInt16) > pabyEnd)
            {
                m_bError = TRUE;
                returnError();
            }

            /* eCurFieldType = OFTInteger; */
            m_sCurField.Integer = GetInt16(m_pabyIterVals, 0);

            m_pabyIterVals += sizeof(GInt16);
            /* CPLDebug("OpenFileGDB", "Field %d, row %d: %d", iCol, nCurRow,
             * sCurField.Integer); */

            break;
        }

        case FGFT_INT32:
        {
            if (m_pabyIterVals + sizeof(GInt32) > pabyEnd)
            {
                m_bError = TRUE;
                returnError();
            }

            /* eCurFieldType = OFTInteger; */
            m_sCurField.Integer = GetInt32(m_pabyIterVals, 0);

            m_pabyIterVals += sizeof(GInt32);
            /* CPLDebug("OpenFileGDB", "Field %d, row %d: %d", iCol, nCurRow,
             * sCurField.Integer); */

            break;
        }

        case FGFT_FLOAT32:
        {
            if (m_pabyIterVals + sizeof(float) > pabyEnd)
            {
                m_bError = TRUE;
                returnError();
            }

            /* eCurFieldType = OFTReal; */
            m_sCurField.Real = GetFloat32(m_pabyIterVals, 0);

            m_pabyIterVals += sizeof(float);
            /* CPLDebug("OpenFileGDB", "Field %d, row %d: %f", iCol, nCurRow,
             * sCurField.Real); */

            break;
        }

        case FGFT_FLOAT64:
        {
            if (m_pabyIterVals + sizeof(double) > pabyEnd)
            {
                m_bError = TRUE;
                returnError();
            }

            /* eCurFieldType = OFTReal; */
            m_sCurField.Real = GetFloat64(m_pabyIterVals, 0);

            m_pabyIterVals += sizeof(double);
            /* CPLDebug("OpenFileGDB", "Field %d, row %d: %f", iCol, nCurRow,
             * sCurField.Real); */

            break;
        }

        case FGFT_DATETIME:
        case FGFT_DATE:
        {
            if (m_pabyIterVals + sizeof(double) > pabyEnd)
            {
                m_bError = TRUE;
                returnError();
            }

            /* Number of days since 1899/12/30 00:00:00 */
            const double dfVal = GetFloat64(m_pabyIterVals, 0);

            if (m_apoFields[iCol]->m_bReadAsDouble)
            {
                m_sCurField.Real = dfVal;
            }
            else
            {
                FileGDBDoubleDateToOGRDate(
                    dfVal, m_apoFields[iCol]->IsHighPrecision(), &m_sCurField);
                /* eCurFieldType = OFTDateTime; */
            }

            m_pabyIterVals += sizeof(double);

            break;
        }

        case FGFT_GEOMETRY:
        case FGFT_BINARY:
        {
            GUInt32 nLength;
            if (!ReadVarUInt32(m_pabyIterVals, pabyEnd, nLength))
            {
                m_bError = TRUE;
                returnError();
            }
            if (nLength > static_cast<GUInt32>(pabyEnd - m_pabyIterVals))
            {
                m_bError = TRUE;
                returnError();
            }

            /* eCurFieldType = OFTBinary; */
            m_sCurField.Binary.nCount = nLength;
            m_sCurField.Binary.paData = const_cast<GByte *>(m_pabyIterVals);

            m_pabyIterVals += nLength;

            /* Null terminate binary in case it is used as a string */
            m_nChSaved = *m_pabyIterVals;
            *m_pabyIterVals = '\0';

            /* CPLDebug("OpenFileGDB", "Field %d, row %d: %d bytes", iCol,
             * nCurRow, snLength); */

            break;
        }

        case FGFT_RASTER:
        {
            const FileGDBRasterField *rasterField =
                cpl::down_cast<const FileGDBRasterField *>(
                    m_apoFields[iCol].get());
            if (rasterField->GetRasterType() ==
                FileGDBRasterField::Type::MANAGED)
            {
                if (m_pabyIterVals + sizeof(GInt32) > pabyEnd)
                {
                    m_bError = TRUE;
                    returnError();
                }

                const GInt32 nVal = GetInt32(m_pabyIterVals, 0);

                /* eCurFieldType = OFTIntger; */
                m_sCurField.Integer = nVal;

                m_pabyIterVals += sizeof(GInt32);
            }
            else
            {
                GUInt32 nLength;
                if (!ReadVarUInt32(m_pabyIterVals, pabyEnd, nLength))
                {
                    m_bError = TRUE;
                    returnError();
                }
                if (nLength > static_cast<GUInt32>(pabyEnd - m_pabyIterVals))
                {
                    m_bError = TRUE;
                    returnError();
                }

                if (rasterField->GetRasterType() ==
                    FileGDBRasterField::Type::EXTERNAL)
                {
                    // coverity[tainted_data,tainted_data_argument]
                    m_osCacheRasterFieldPath =
                        ReadUTF16String(m_pabyIterVals, nLength / 2);
                    m_sCurField.String = &m_osCacheRasterFieldPath[0];
                    m_pabyIterVals += nLength;
                }
                else
                {
                    /* eCurFieldType = OFTBinary; */
                    m_sCurField.Binary.nCount = nLength;
                    m_sCurField.Binary.paData =
                        const_cast<GByte *>(m_pabyIterVals);

                    m_pabyIterVals += nLength;

                    /* Null terminate binary in case it is used as a string */
                    m_nChSaved = *m_pabyIterVals;
                    *m_pabyIterVals = '\0';
                }
            }
            break;
        }

        case FGFT_GUID:
        case FGFT_GLOBALID:
        {
            if (m_pabyIterVals + UUID_SIZE_IN_BYTES > pabyEnd)
            {
                m_bError = TRUE;
                returnError();
            }

            /* eCurFieldType = OFTString; */
            m_sCurField.String = m_achGUIDBuffer;
            /*78563412BC9AF0DE1234567890ABCDEF -->
             * {12345678-9ABC-DEF0-1234-567890ABCDEF} */
            snprintf(m_achGUIDBuffer, sizeof(m_achGUIDBuffer),
                     "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%"
                     "02X%02X%02X%02X}",
                     m_pabyIterVals[3], m_pabyIterVals[2], m_pabyIterVals[1],
                     m_pabyIterVals[0], m_pabyIterVals[5], m_pabyIterVals[4],
                     m_pabyIterVals[7], m_pabyIterVals[6], m_pabyIterVals[8],
                     m_pabyIterVals[9], m_pabyIterVals[10], m_pabyIterVals[11],
                     m_pabyIterVals[12], m_pabyIterVals[13], m_pabyIterVals[14],
                     m_pabyIterVals[15]);

            m_pabyIterVals += UUID_SIZE_IN_BYTES;
            /* CPLDebug("OpenFileGDB", "Field %d, row %d: %s", iCol, nCurRow,
             * sCurField.String); */

            break;
        }

        case FGFT_INT64:
        {
            if (m_pabyIterVals + sizeof(int64_t) > pabyEnd)
            {
                m_bError = TRUE;
                returnError();
            }

            /* eCurFieldType = OFTInteger; */
            m_sCurField.Integer64 = GetInt64(m_pabyIterVals, 0);

            m_pabyIterVals += sizeof(int64_t);
            /* CPLDebug("OpenFileGDB", "Field %d, row %d: " CPL_FRMT_GIB, iCol, nCurRow,
             * sCurField.Integer64); */

            break;
        }

        case FGFT_TIME:
        {
            if (m_pabyIterVals + sizeof(double) > pabyEnd)
            {
                m_bError = TRUE;
                returnError();
            }

            /* Fraction of day */
            const double dfVal = GetFloat64(m_pabyIterVals, 0);

            if (m_apoFields[iCol]->m_bReadAsDouble)
            {
                m_sCurField.Real = dfVal;
            }
            else
            {
                FileGDBDoubleTimeToOGRTime(dfVal, &m_sCurField);
                /* eCurFieldType = OFTTime; */
            }

            m_pabyIterVals += sizeof(double);

            break;
        }

        case FGFT_DATETIME_WITH_OFFSET:
        {
            if (m_pabyIterVals + sizeof(double) + sizeof(int16_t) > pabyEnd)
            {
                m_bError = TRUE;
                returnError();
            }

            /* Number of days since 1899/12/30 00:00:00 */
            const double dfVal = GetFloat64(m_pabyIterVals, 0);
            m_pabyIterVals += sizeof(double);
            const int16_t nUTCOffset = GetInt16(m_pabyIterVals, 0);
            m_pabyIterVals += sizeof(int16_t);

            if (m_apoFields[iCol]->m_bReadAsDouble)
            {
                m_sCurField.Real = dfVal - nUTCOffset * 60.0 / 86400.0;
            }
            else
            {
                FileGDBDateTimeWithOffsetToOGRDate(dfVal, nUTCOffset,
                                                   &m_sCurField);
                /* eCurFieldType = OFTDateTime; */
            }

            break;
        }
    }

    if (iCol == static_cast<int>(m_apoFields.size()) - 1 &&
        m_pabyIterVals < pabyEnd)
    {
        CPLDebug("OpenFileGDB", "%d bytes remaining at end of record %" PRId64,
                 static_cast<int>(pabyEnd - m_pabyIterVals), m_nCurRow);
    }

    return &m_sCurField;
}

/************************************************************************/
/*                           GetIndexCount()                            */
/************************************************************************/

int FileGDBTable::GetIndexCount()
{
    const int errorRetValue = 0;
    if (m_bHasReadGDBIndexes)
        return static_cast<int>(m_apoIndexes.size());

    m_bHasReadGDBIndexes = TRUE;

    const std::string osIndexesName = CPLFormFilenameSafe(
        CPLGetPathSafe(m_osFilename.c_str()).c_str(),
        CPLGetBasenameSafe(m_osFilename.c_str()).c_str(), "gdbindexes");
    VSILFILE *fpIndexes = VSIFOpenL(osIndexesName.c_str(), "rb");
    VSIStatBufL sStat;
    if (fpIndexes == nullptr)
    {
        if (VSIStatExL(osIndexesName.c_str(), &sStat, VSI_STAT_EXISTS_FLAG) ==
            0)
            returnError();
        else
            return 0;
    }

    VSIFSeekL(fpIndexes, 0, SEEK_END);
    vsi_l_offset nFileSize = VSIFTellL(fpIndexes);
    returnErrorAndCleanupIf(nFileSize > 1024 * 1024, VSIFCloseL(fpIndexes));

    GByte *pabyIdx = static_cast<GByte *>(
        VSI_MALLOC_VERBOSE(static_cast<size_t>(nFileSize)));
    returnErrorAndCleanupIf(pabyIdx == nullptr, VSIFCloseL(fpIndexes));

    VSIFSeekL(fpIndexes, 0, SEEK_SET);
    int nRead = static_cast<int>(
        VSIFReadL(pabyIdx, static_cast<size_t>(nFileSize), 1, fpIndexes));
    VSIFCloseL(fpIndexes);
    returnErrorAndCleanupIf(nRead != 1, VSIFree(pabyIdx));

    GByte *pabyCur = pabyIdx;
    GByte *pabyEnd = pabyIdx + nFileSize;
    returnErrorAndCleanupIf(pabyEnd - pabyCur < 4, VSIFree(pabyIdx));
    GUInt32 nIndexCount = GetUInt32(pabyCur, 0);
    pabyCur += 4;

    // FileGDB v9 indexes structure not handled yet. Start with 13 98 85 03
    if (nIndexCount == 0x03859813)
    {
        VSIFree(pabyIdx);

        // Hard code detection of blk_key_index on raster layers
        const int iBlockKeyFieldIdx = GetFieldIdx("block_key");
        if (iBlockKeyFieldIdx >= 0)
        {
            const std::string osAtxFilename = CPLResetExtensionSafe(
                m_osFilename.c_str(), "blk_key_index.atx");
            if (VSIStatExL(osAtxFilename.c_str(), &sStat,
                           VSI_STAT_EXISTS_FLAG) == 0)
            {
                auto poIndex = std::make_unique<FileGDBIndex>();
                poIndex->m_osIndexName = "blk_key_index";
                poIndex->m_osExpression = "block_key";
                m_apoFields[iBlockKeyFieldIdx]->m_poIndex = poIndex.get();
                m_apoIndexes.push_back(std::move(poIndex));
                return 1;
            }
        }

        CPLDebug("OpenFileGDB", ".gdbindexes v9 not handled yet");
        return 0;
    }

    returnErrorAndCleanupIf(nIndexCount >=
                                static_cast<size_t>(GetFieldCount() + 1) * 10,
                            VSIFree(pabyIdx));

    GUInt32 i;
    for (i = 0; i < nIndexCount; i++)
    {
        returnErrorAndCleanupIf(static_cast<size_t>(pabyEnd - pabyCur) <
                                    sizeof(GUInt32),
                                VSIFree(pabyIdx));
        const GUInt32 nIdxNameCharCount = GetUInt32(pabyCur, 0);
        pabyCur += sizeof(GUInt32);
        returnErrorAndCleanupIf(nIdxNameCharCount > 1024, VSIFree(pabyIdx));
        returnErrorAndCleanupIf(static_cast<size_t>(pabyEnd - pabyCur) <
                                    2 * nIdxNameCharCount,
                                VSIFree(pabyIdx));
        std::string osIndexName(ReadUTF16String(pabyCur, nIdxNameCharCount));
        pabyCur += 2 * nIdxNameCharCount;

        // 4 "magic fields"
        returnErrorAndCleanupIf(static_cast<size_t>(pabyEnd - pabyCur) <
                                    sizeof(GUInt16) + sizeof(GUInt32) +
                                        sizeof(GUInt16) + sizeof(GUInt32),
                                VSIFree(pabyIdx));
        // const GUInt16 nMagic1 = GetUInt16(pabyCur, 0);
        const GUInt32 nMagic2 = GetUInt32(pabyCur + sizeof(GUInt16), 0);
        const GUInt16 nMagic3 =
            GetUInt16(pabyCur + sizeof(GUInt16) + sizeof(GUInt32), 0);
        if (!((nMagic2 == 2 && nMagic3 == 0) ||
              (nMagic2 == 4 && nMagic3 == 0) ||
              (nMagic2 == 16 && nMagic3 == 65535)))
        {
            // Cf files a00000029.gdbindexes, a000000ea.gdbindexes, a000000ed.gdbindexes,
            // a000000f8.gdbindexes, a000000fb.gdbindexes, a00000103.gdbindexes
            // from https://github.com/OSGeo/gdal/issues/11295#issuecomment-2491158506
            CPLDebug("OpenFileGDB", "Reading %s", osIndexesName.c_str());
            CPLDebug(
                "OpenFileGDB",
                "Strange (deleted?) index descriptor at index %u of name %s", i,
                osIndexName.c_str());

            // Skip magic fields
            pabyCur += sizeof(GUInt16);

            const GUInt32 nColNameCharCount = nMagic2;
            pabyCur += sizeof(GUInt32);
            returnErrorAndCleanupIf(nColNameCharCount > 1024, VSIFree(pabyIdx));
            returnErrorAndCleanupIf(static_cast<size_t>(pabyEnd - pabyCur) <
                                        2 * nColNameCharCount,
                                    VSIFree(pabyIdx));
            pabyCur += 2 * nColNameCharCount;

            // Skip magic field
            returnErrorAndCleanupIf(static_cast<size_t>(pabyEnd - pabyCur) <
                                        sizeof(GUInt16),
                                    VSIFree(pabyIdx));
            pabyCur += sizeof(GUInt16);

            continue;
        }

        // Skip magic fields
        pabyCur += sizeof(GUInt16) + sizeof(GUInt32) + sizeof(GUInt16) +
                   sizeof(GUInt32);

        returnErrorAndCleanupIf(static_cast<size_t>(pabyEnd - pabyCur) <
                                    sizeof(GUInt32),
                                VSIFree(pabyIdx));
        const GUInt32 nColNameCharCount = GetUInt32(pabyCur, 0);
        pabyCur += sizeof(GUInt32);
        returnErrorAndCleanupIf(nColNameCharCount > 1024, VSIFree(pabyIdx));
        returnErrorAndCleanupIf(static_cast<size_t>(pabyEnd - pabyCur) <
                                    2 * nColNameCharCount,
                                VSIFree(pabyIdx));
        const std::string osExpression(
            ReadUTF16String(pabyCur, nColNameCharCount));
        pabyCur += 2 * nColNameCharCount;

        // Skip magic field
        returnErrorAndCleanupIf(static_cast<size_t>(pabyEnd - pabyCur) <
                                    sizeof(GUInt16),
                                VSIFree(pabyIdx));
        pabyCur += sizeof(GUInt16);

        auto poIndex = std::make_unique<FileGDBIndex>();
        poIndex->m_osIndexName = std::move(osIndexName);
        poIndex->m_osExpression = osExpression;

        if (m_iObjectIdField < 0 ||
            osExpression != m_apoFields[m_iObjectIdField]->GetName())
        {
            const auto osFieldName = poIndex->GetFieldName();
            int nFieldIdx = GetFieldIdx(osFieldName);
            if (nFieldIdx < 0)
            {
                CPLDebug("OpenFileGDB",
                         "Index defined for field %s that does not exist",
                         osFieldName.c_str());
            }
            else
            {
                if (m_apoFields[nFieldIdx]->m_poIndex != nullptr)
                {
                    CPLDebug("OpenFileGDB",
                             "There is already one index defined for field %s",
                             osFieldName.c_str());
                }
                else
                {
                    m_apoFields[nFieldIdx]->m_poIndex = poIndex.get();
                }
            }
        }

        m_apoIndexes.push_back(std::move(poIndex));
    }

    VSIFree(pabyIdx);

    return static_cast<int>(m_apoIndexes.size());
}

/************************************************************************/
/*                           HasSpatialIndex()                          */
/************************************************************************/

bool FileGDBTable::HasSpatialIndex()
{
    if (m_nHasSpatialIndex < 0)
    {
        const std::string osSpxName = CPLFormFilenameSafe(
            CPLGetPathSafe(m_osFilename.c_str()).c_str(),
            CPLGetBasenameSafe(m_osFilename.c_str()).c_str(), "spx");
        VSIStatBufL sStat;
        m_nHasSpatialIndex =
            (VSIStatExL(osSpxName.c_str(), &sStat, VSI_STAT_EXISTS_FLAG) == 0);
    }
    return m_nHasSpatialIndex != FALSE;
}

/************************************************************************/
/*                       InstallFilterEnvelope()                        */
/************************************************************************/

#define MAX_GUINTBIG std::numeric_limits<GUIntBig>::max()

void FileGDBTable::InstallFilterEnvelope(const OGREnvelope *psFilterEnvelope)
{
    if (psFilterEnvelope != nullptr)
    {
        CPLAssert(m_iGeomField >= 0);
        FileGDBGeomField *poGeomField =
            cpl::down_cast<FileGDBGeomField *>(GetField(m_iGeomField));

        /* We store the bounding box as unscaled coordinates, so that BBOX */
        /* intersection is done with integer comparisons */
        if (psFilterEnvelope->MinX >= poGeomField->m_dfXOrigin)
            m_nFilterXMin = static_cast<GUIntBig>(
                0.5 + (psFilterEnvelope->MinX - poGeomField->m_dfXOrigin) *
                          poGeomField->m_dfXYScale);
        else
            m_nFilterXMin = 0;
        if (psFilterEnvelope->MaxX - poGeomField->m_dfXOrigin <
            static_cast<double>(MAX_GUINTBIG) / poGeomField->m_dfXYScale)
            m_nFilterXMax = static_cast<GUIntBig>(
                0.5 + (psFilterEnvelope->MaxX - poGeomField->m_dfXOrigin) *
                          poGeomField->m_dfXYScale);
        else
            m_nFilterXMax = MAX_GUINTBIG;
        if (psFilterEnvelope->MinY >= poGeomField->m_dfYOrigin)
            m_nFilterYMin = static_cast<GUIntBig>(
                0.5 + (psFilterEnvelope->MinY - poGeomField->m_dfYOrigin) *
                          poGeomField->m_dfXYScale);
        else
            m_nFilterYMin = 0;
        if (psFilterEnvelope->MaxY - poGeomField->m_dfYOrigin <
            static_cast<double>(MAX_GUINTBIG) / poGeomField->m_dfXYScale)
            m_nFilterYMax = static_cast<GUIntBig>(
                0.5 + (psFilterEnvelope->MaxY - poGeomField->m_dfYOrigin) *
                          poGeomField->m_dfXYScale);
        else
            m_nFilterYMax = MAX_GUINTBIG;
    }
    else
    {
        m_nFilterXMin = 0;
        m_nFilterXMax = 0;
        m_nFilterYMin = 0;
        m_nFilterYMax = 0;
    }
}

/************************************************************************/
/*                  GetMinMaxProjYForSpatialIndex()                     */
/************************************************************************/

// ESRI software seems to have an extremely weird behavior regarding spatial
// indexing of geometries.
// When a projected CRS is associated with a layer, the northing of geometries
// is clamped, using the returned (dfYMin,dfYMax) values of this method.
// When creating the .spx file, if the maximum Y of a geometry is > dfYMax, then
// the geometry must be shifted along the Y axis so that its maximum value is
// dfYMax
void FileGDBTable::GetMinMaxProjYForSpatialIndex(double &dfYMin,
                                                 double &dfYMax) const
{
    dfYMin = -std::numeric_limits<double>::max();
    dfYMax = std::numeric_limits<double>::max();
    const auto poGeomField = GetGeomField();
    if (poGeomField == nullptr)
        return;
    const auto &osWKT = poGeomField->GetWKT();
    OGRSpatialReference oSRS;
    if (osWKT.empty() || osWKT[0] == '{' ||
        oSRS.importFromWkt(osWKT.c_str()) != OGRERR_NONE)
        return;
    if (!oSRS.IsProjected())
        return;
    const char *pszProjection = oSRS.GetAttrValue("PROJECTION");
    if (pszProjection == nullptr)
        return;
    double dfMinLat;
    double dfMaxLat;

    // Determined through experimentation, e.g with the `find_srs_latitude_limits.py` script.
    if (EQUAL(pszProjection, SRS_PT_TRANSVERSE_MERCATOR))
    {
        dfMinLat = -90;
        dfMaxLat = 90;
    }
    else if (EQUAL(pszProjection, SRS_PT_MERCATOR_2SP) ||
             EQUAL(pszProjection, SRS_PT_MERCATOR_1SP))
    {
        dfMinLat = -89.9;
        dfMaxLat = 89.9;
    }
    else
    {
        // TODO? add other projection methods
        return;
    }

    auto poSRSLongLat =
        std::unique_ptr<OGRSpatialReference>(oSRS.CloneGeogCS());
    auto poCT = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(poSRSLongLat.get(), &oSRS));
    if (!poCT)
        return;
    {
        double x = 0;
        double y = dfMinLat;
        if (poCT->Transform(1, &x, &y))
            dfYMin = y;
    }
    {
        double x = 0;
        double y = dfMaxLat;
        if (poCT->Transform(1, &x, &y))
            dfYMax = y;
    }
}

/************************************************************************/
/*                         GetFeatureExtent()                           */
/************************************************************************/

int FileGDBTable::GetFeatureExtent(const OGRField *psField,
                                   OGREnvelope *psOutFeatureEnvelope)
{
    const int errorRetValue = FALSE;
    GByte *pabyCur = psField->Binary.paData;
    GByte *pabyEnd = pabyCur + psField->Binary.nCount;
    GUInt32 nGeomType;
    int nToSkip = 0;

    CPLAssert(m_iGeomField >= 0);
    FileGDBGeomField *poGeomField =
        cpl::down_cast<FileGDBGeomField *>(GetField(m_iGeomField));

    ReadVarUInt32NoCheck(pabyCur, nGeomType);

    switch ((nGeomType & 0xff))
    {
        case SHPT_NULL:
            return FALSE;

        case SHPT_POINTZ:
        case SHPT_POINTZM:
        case SHPT_POINT:
        case SHPT_POINTM:
        case SHPT_GENERALPOINT:
        {
            GUIntBig x, y;
            ReadVarUInt64NoCheck(pabyCur, x);
            x = CPLUnsanitizedAdd<GUIntBig>(x, -1);
            ReadVarUInt64NoCheck(pabyCur, y);
            y = CPLUnsanitizedAdd<GUIntBig>(y, -1);
            psOutFeatureEnvelope->MinX =
                x / poGeomField->m_dfXYScale + poGeomField->m_dfXOrigin;
            psOutFeatureEnvelope->MinY =
                y / poGeomField->m_dfXYScale + poGeomField->m_dfYOrigin;
            psOutFeatureEnvelope->MaxX = psOutFeatureEnvelope->MinX;
            psOutFeatureEnvelope->MaxY = psOutFeatureEnvelope->MinY;
            return TRUE;
        }

        case SHPT_MULTIPOINTZM:
        case SHPT_MULTIPOINTZ:
        case SHPT_MULTIPOINT:
        case SHPT_MULTIPOINTM:
        {
            break;
        }

        case SHPT_ARC:
        case SHPT_ARCZ:
        case SHPT_ARCZM:
        case SHPT_ARCM:
        case SHPT_POLYGON:
        case SHPT_POLYGONZ:
        case SHPT_POLYGONZM:
        case SHPT_POLYGONM:
        {
            nToSkip = 1;
            break;
        }
        case SHPT_GENERALPOLYLINE:
        case SHPT_GENERALPOLYGON:
        {
            nToSkip = 1 + ((nGeomType & EXT_SHAPE_CURVE_FLAG) ? 1 : 0);
            break;
        }

        case SHPT_GENERALMULTIPATCH:
        case SHPT_MULTIPATCHM:
        case SHPT_MULTIPATCH:
        {
            nToSkip = 2;
            break;
        }

        default:
            return FALSE;
    }

    GUInt32 nPoints;
    ReadVarUInt32NoCheck(pabyCur, nPoints);
    if (nPoints == 0)
        return TRUE;
    returnErrorIf(!SkipVarUInt(pabyCur, pabyEnd, nToSkip));

    GUIntBig vxmin, vymin, vdx, vdy;

    returnErrorIf(pabyCur >= pabyEnd);
    ReadVarUInt64NoCheck(pabyCur, vxmin);
    ReadVarUInt64NoCheck(pabyCur, vymin);
    ReadVarUInt64NoCheck(pabyCur, vdx);
    ReadVarUInt64NoCheck(pabyCur, vdy);

    psOutFeatureEnvelope->MinX =
        vxmin / poGeomField->m_dfXYScale + poGeomField->m_dfXOrigin;
    psOutFeatureEnvelope->MinY =
        vymin / poGeomField->m_dfXYScale + poGeomField->m_dfYOrigin;
    psOutFeatureEnvelope->MaxX =
        CPLUnsanitizedAdd<GUIntBig>(vxmin, vdx) / poGeomField->m_dfXYScale +
        poGeomField->m_dfXOrigin;
    psOutFeatureEnvelope->MaxY =
        CPLUnsanitizedAdd<GUIntBig>(vymin, vdy) / poGeomField->m_dfXYScale +
        poGeomField->m_dfYOrigin;

    return TRUE;
}

/************************************************************************/
/*                 DoesGeometryIntersectsFilterEnvelope()               */
/************************************************************************/

int FileGDBTable::DoesGeometryIntersectsFilterEnvelope(const OGRField *psField)
{
    const int errorRetValue = TRUE;
    GByte *pabyCur = psField->Binary.paData;
    GByte *pabyEnd = pabyCur + psField->Binary.nCount;
    GUInt32 nGeomType;
    int nToSkip = 0;

    ReadVarUInt32NoCheck(pabyCur, nGeomType);

    switch ((nGeomType & 0xff))
    {
        case SHPT_NULL:
            return TRUE;

        case SHPT_POINTZ:
        case SHPT_POINTZM:
        case SHPT_POINT:
        case SHPT_POINTM:
        case SHPT_GENERALPOINT:
        {
            GUIntBig x, y;
            ReadVarUInt64NoCheck(pabyCur, x);
            if (x == 0)  // POINT EMPTY
                return FALSE;
            x--;
            if (x < m_nFilterXMin || x > m_nFilterXMax)
                return FALSE;
            ReadVarUInt64NoCheck(pabyCur, y);
            y--;
            return y >= m_nFilterYMin && y <= m_nFilterYMax;
        }

        case SHPT_MULTIPOINTZM:
        case SHPT_MULTIPOINTZ:
        case SHPT_MULTIPOINT:
        case SHPT_MULTIPOINTM:
        {
            break;
        }

        case SHPT_ARC:
        case SHPT_ARCZ:
        case SHPT_ARCZM:
        case SHPT_ARCM:
        case SHPT_POLYGON:
        case SHPT_POLYGONZ:
        case SHPT_POLYGONZM:
        case SHPT_POLYGONM:
        {
            nToSkip = 1;
            break;
        }

        case SHPT_GENERALPOLYLINE:
        case SHPT_GENERALPOLYGON:
        {
            nToSkip = 1 + ((nGeomType & EXT_SHAPE_CURVE_FLAG) ? 1 : 0);
            break;
        }

        case SHPT_GENERALMULTIPATCH:
        case SHPT_MULTIPATCHM:
        case SHPT_MULTIPATCH:
        {
            nToSkip = 2;
            break;
        }

        default:
            return TRUE;
    }

    GUInt32 nPoints;
    ReadVarUInt32NoCheck(pabyCur, nPoints);
    if (nPoints == 0)
        return TRUE;
    returnErrorIf(!SkipVarUInt(pabyCur, pabyEnd, nToSkip));

    GUIntBig vxmin, vymin, vdx, vdy;

    returnErrorIf(pabyCur >= pabyEnd);
    ReadVarUInt64NoCheck(pabyCur, vxmin);
    if (vxmin > m_nFilterXMax)
        return FALSE;
    ReadVarUInt64NoCheck(pabyCur, vymin);
    if (vymin > m_nFilterYMax)
        return FALSE;
    ReadVarUInt64NoCheck(pabyCur, vdx);
    if (CPLUnsanitizedAdd<GUIntBig>(vxmin, vdx) < m_nFilterXMin)
        return FALSE;
    ReadVarUInt64NoCheck(pabyCur, vdy);
    return CPLUnsanitizedAdd<GUIntBig>(vymin, vdy) >= m_nFilterYMin;
}

/************************************************************************/
/*                      FileGDBField::UNSET_FIELD                       */
/************************************************************************/

static OGRField GetUnsetField()
{
    OGRField sUnsetField;
    OGR_RawField_SetUnset(&sUnsetField);
    return sUnsetField;
}

const OGRField FileGDBField::UNSET_FIELD = GetUnsetField();

/************************************************************************/
/*                           FileGDBField()                             */
/************************************************************************/

FileGDBField::FileGDBField(FileGDBTable *poParentIn) : m_poParent(poParentIn)
{
    OGR_RawField_SetUnset(&m_sDefault);
}

/************************************************************************/
/*                           FileGDBField()                             */
/************************************************************************/

FileGDBField::FileGDBField(const std::string &osName,
                           const std::string &osAlias, FileGDBFieldType eType,
                           bool bNullable, bool bRequired, bool bEditable,
                           int nMaxWidth, const OGRField &sDefault)
    : m_osName(osName), m_osAlias(osAlias), m_eType(eType),
      m_bNullable(bNullable), m_bRequired(bRequired), m_bEditable(bEditable),
      m_nMaxWidth(nMaxWidth)
{
    if (m_eType == FGFT_OBJECTID || m_eType == FGFT_GLOBALID)
    {
        CPLAssert(!m_bNullable);
        CPLAssert(m_bRequired);
        CPLAssert(!m_bEditable);
    }

    if (m_eType == FGFT_STRING && !OGR_RawField_IsUnset(&sDefault) &&
        !OGR_RawField_IsNull(&sDefault))
    {
        m_sDefault.String = CPLStrdup(sDefault.String);
    }
    else
    {
        m_sDefault = sDefault;
    }
}

/************************************************************************/
/*                          ~FileGDBField()                             */
/************************************************************************/

FileGDBField::~FileGDBField()
{
    if (m_eType == FGFT_STRING && !OGR_RawField_IsUnset(&m_sDefault) &&
        !OGR_RawField_IsNull(&m_sDefault))
        CPLFree(m_sDefault.String);
}

/************************************************************************/
/*                            HasIndex()                                */
/************************************************************************/

int FileGDBField::HasIndex()
{
    m_poParent->GetIndexCount();
    return m_poIndex != nullptr;
}

/************************************************************************/
/*                            GetIndex()                                */
/************************************************************************/

FileGDBIndex *FileGDBField::GetIndex()
{
    m_poParent->GetIndexCount();
    return m_poIndex;
}

/************************************************************************/
/*                              getESRI_NAN()                           */
/************************************************************************/

static double getESRI_NAN()
{
    // Use exact same quiet NaN value as generated by the ESRI SDK, just
    // for the purpose of ensuring binary identical output for some tests.
    // I doubt this matter much which NaN is generated for usage.
    // The reason is that std::numeric_limits<double>::quiet_NaN() on my
    // platform has not the least significant bit set.
    constexpr uint64_t nNAN = (static_cast<uint64_t>(0x7FF80000U) << 32) | 1;
    double v;
    memcpy(&v, &nNAN, sizeof(v));
    return v;
}

const double FileGDBGeomField::ESRI_NAN = getESRI_NAN();

/************************************************************************/
/*                           FileGDBGeomField()                         */
/************************************************************************/

FileGDBGeomField::FileGDBGeomField(FileGDBTable *poParentIn)
    : FileGDBField(poParentIn)
{
}

/************************************************************************/
/*                           FileGDBGeomField()                         */
/************************************************************************/

FileGDBGeomField::FileGDBGeomField(
    const std::string &osName, const std::string &osAlias, bool bNullable,
    const std::string &osWKT, double dfXOrigin, double dfYOrigin,
    double dfXYScale, double dfXYTolerance,
    const std::vector<double> &adfSpatialIndexGridResolution)
    : FileGDBField(osName, osAlias, FGFT_GEOMETRY, bNullable,
                   /* bRequired = */ true, /* bEditable = */ true, 0,
                   FileGDBField::UNSET_FIELD),
      m_osWKT(osWKT), m_dfXOrigin(dfXOrigin), m_dfYOrigin(dfYOrigin),
      m_dfXYScale(dfXYScale), m_dfXYTolerance(dfXYTolerance),
      m_adfSpatialIndexGridResolution(adfSpatialIndexGridResolution)
{
}

/************************************************************************/
/*                                SetXYMinMax()                         */
/************************************************************************/

void FileGDBGeomField::SetXYMinMax(double dfXMin, double dfYMin, double dfXMax,
                                   double dfYMax)
{
    m_dfXMin = dfXMin;
    m_dfYMin = dfYMin;
    m_dfXMax = dfXMax;
    m_dfYMax = dfYMax;
}

/************************************************************************/
/*                                SetZMinMax()                          */
/************************************************************************/

void FileGDBGeomField::SetZMinMax(double dfZMin, double dfZMax)
{
    m_dfZMin = dfZMin;
    m_dfZMax = dfZMax;
}

/************************************************************************/
/*                                SetMMinMax()                          */
/************************************************************************/

void FileGDBGeomField::SetMMinMax(double dfMMin, double dfMMax)
{
    m_dfMMin = dfMMin;
    m_dfMMax = dfMMax;
}

/************************************************************************/
/*                      SetZOriginScaleTolerance()                      */
/************************************************************************/

void FileGDBGeomField::SetZOriginScaleTolerance(double dfZOrigin,
                                                double dfZScale,
                                                double dfZTolerance)
{
    m_bHasZOriginScaleTolerance = TRUE;
    m_dfZOrigin = dfZOrigin;
    m_dfZScale = dfZScale;
    m_dfZTolerance = dfZTolerance;
}

/************************************************************************/
/*                      SetMOriginScaleTolerance()                      */
/************************************************************************/

void FileGDBGeomField::SetMOriginScaleTolerance(double dfMOrigin,
                                                double dfMScale,
                                                double dfMTolerance)
{
    m_bHasMOriginScaleTolerance = TRUE;
    m_dfMOrigin = dfMOrigin;
    m_dfMScale = dfMScale;
    m_dfMTolerance = dfMTolerance;
}

FileGDBOGRGeometryConverter::~FileGDBOGRGeometryConverter() = default;

/************************************************************************/
/*                      FileGDBOGRGeometryConverterImpl                 */
/************************************************************************/

class FileGDBOGRGeometryConverterImpl final : public FileGDBOGRGeometryConverter
{
    const FileGDBGeomField *poGeomField;
    GUInt32 *panPointCount = nullptr;
    GUInt32 nPointCountMax = 0;
#ifdef ASSUME_INNER_RINGS_IMMEDIATELY_AFTER_OUTER_RING
    int bUseOrganize = 0;
#endif

    bool ReadPartDefs(GByte *&pabyCur, GByte *pabyEnd, GUInt32 &nPoints,
                      GUInt32 &nParts, GUInt32 &nCurves, bool bHasCurveDesc,
                      bool bIsMultiPatch);
    template <class XYSetter>
    int ReadXYArray(XYSetter &setter, GByte *&pabyCur, GByte *pabyEnd,
                    GUInt32 nPoints, GIntBig &dx, GIntBig &dy);
    template <class ZSetter>
    int ReadZArray(ZSetter &setter, GByte *&pabyCur, GByte *pabyEnd,
                   GUInt32 nPoints, GIntBig &dz);
    template <class MSetter>
    int ReadMArray(MSetter &setter, GByte *&pabyCur, GByte *pabyEnd,
                   GUInt32 nPoints, GIntBig &dm);

    OGRGeometry *CreateCurveGeometry(GUInt32 nBaseShapeType, GUInt32 nParts,
                                     GUInt32 nPoints, GUInt32 nCurves,
                                     bool bHasZ, bool bHasM, GByte *&pabyCur,
                                     GByte *pabyEnd);

    FileGDBOGRGeometryConverterImpl(const FileGDBOGRGeometryConverterImpl &) =
        delete;
    FileGDBOGRGeometryConverterImpl &
    operator=(const FileGDBOGRGeometryConverterImpl &) = delete;

  public:
    explicit FileGDBOGRGeometryConverterImpl(
        const FileGDBGeomField *poGeomField);
    virtual ~FileGDBOGRGeometryConverterImpl();

    virtual OGRGeometry *GetAsGeometry(const OGRField *psField) override;
};

/************************************************************************/
/*                  FileGDBOGRGeometryConverterImpl()                   */
/************************************************************************/

FileGDBOGRGeometryConverterImpl::FileGDBOGRGeometryConverterImpl(
    const FileGDBGeomField *poGeomFieldIn)
    : poGeomField(poGeomFieldIn)
#ifdef ASSUME_INNER_RINGS_IMMEDIATELY_AFTER_OUTER_RING
      ,
      bUseOrganize(CPLGetConfigOption("OGR_ORGANIZE_POLYGONS", NULL) != NULL)
#endif
{
}

/************************************************************************/
/*                 ~FileGDBOGRGeometryConverter()                       */
/************************************************************************/

FileGDBOGRGeometryConverterImpl::~FileGDBOGRGeometryConverterImpl()
{
    CPLFree(panPointCount);
}

/************************************************************************/
/*                          ReadPartDefs()                              */
/************************************************************************/

bool FileGDBOGRGeometryConverterImpl::ReadPartDefs(
    GByte *&pabyCur, GByte *pabyEnd, GUInt32 &nPoints, GUInt32 &nParts,
    GUInt32 &nCurves, bool bHasCurveDesc, bool bIsMultiPatch)
{
    const bool errorRetValue = false;
    returnErrorIf(!ReadVarUInt32(pabyCur, pabyEnd, nPoints));
    if (nPoints == 0)
    {
        nParts = 0;
        nCurves = 0;
        return true;
    }
    returnErrorIf(nPoints > static_cast<size_t>(pabyEnd - pabyCur));
    if (bIsMultiPatch)
        returnErrorIf(!SkipVarUInt(pabyCur, pabyEnd));
    returnErrorIf(!ReadVarUInt32(pabyCur, pabyEnd, nParts));
    returnErrorIf(nParts > static_cast<size_t>(pabyEnd - pabyCur));
    returnErrorIf(nParts > static_cast<GUInt32>(INT_MAX) / sizeof(GUInt32) - 1);
    if (bHasCurveDesc)
    {
        returnErrorIf(!ReadVarUInt32(pabyCur, pabyEnd, nCurves));
        returnErrorIf(nCurves > static_cast<size_t>(pabyEnd - pabyCur));
    }
    else
        nCurves = 0;
    if (nParts == 0)
        return true;
    GUInt32 i;
    returnErrorIf(!SkipVarUInt(pabyCur, pabyEnd, 4));
    if (nParts > nPointCountMax)
    {
        GUInt32 *panPointCountNew = static_cast<GUInt32 *>(
            VSI_REALLOC_VERBOSE(panPointCount, nParts * sizeof(GUInt32)));
        returnErrorIf(panPointCountNew == nullptr);
        panPointCount = panPointCountNew;
        nPointCountMax = nParts;
    }
    GUIntBig nSumNPartsM1 = 0;
    for (i = 0; i < nParts - 1; i++)
    {
        GUInt32 nTmp;
        returnErrorIf(!ReadVarUInt32(pabyCur, pabyEnd, nTmp));
        returnErrorIf(nTmp > static_cast<size_t>(pabyEnd - pabyCur));
        panPointCount[i] = nTmp;
        nSumNPartsM1 += nTmp;
    }
    returnErrorIf(nSumNPartsM1 > nPoints);
    panPointCount[nParts - 1] = static_cast<GUInt32>(nPoints - nSumNPartsM1);

    return true;
}

/************************************************************************/
/*                         XYLineStringSetter                           */
/************************************************************************/

class FileGDBOGRLineString : public OGRLineString
{
  public:
    FileGDBOGRLineString() = default;

    ~FileGDBOGRLineString() override;

    OGRRawPoint *GetPoints() const
    {
        return paoPoints;
    }
};

FileGDBOGRLineString::~FileGDBOGRLineString() = default;

class FileGDBOGRLinearRing : public OGRLinearRing
{
  public:
    FileGDBOGRLinearRing() = default;

    ~FileGDBOGRLinearRing() override;

    OGRRawPoint *GetPoints() const
    {
        return paoPoints;
    }
};

FileGDBOGRLinearRing::~FileGDBOGRLinearRing() = default;

class XYLineStringSetter
{
    OGRRawPoint *paoPoints;

  public:
    explicit XYLineStringSetter(OGRRawPoint *paoPointsIn)
        : paoPoints(paoPointsIn)
    {
    }

    void set(int i, double dfX, double dfY)
    {
        paoPoints[i].x = dfX;
        paoPoints[i].y = dfY;
    }
};

/************************************************************************/
/*                         XYMultiPointSetter                           */
/************************************************************************/

class XYMultiPointSetter
{
    OGRMultiPoint *poMPoint;

  public:
    explicit XYMultiPointSetter(OGRMultiPoint *poMPointIn)
        : poMPoint(poMPointIn)
    {
    }

    void set(int i, double dfX, double dfY)
    {
        (void)i;
        poMPoint->addGeometryDirectly(new OGRPoint(dfX, dfY));
    }
};

/************************************************************************/
/*                             XYArraySetter                            */
/************************************************************************/

class XYArraySetter
{
    double *padfX;
    double *padfY;

  public:
    XYArraySetter(double *padfXIn, double *padfYIn)
        : padfX(padfXIn), padfY(padfYIn)
    {
    }

    void set(int i, double dfX, double dfY)
    {
        padfX[i] = dfX;
        padfY[i] = dfY;
    }
};

/************************************************************************/
/*                          ReadXYArray()                               */
/************************************************************************/

template <class XYSetter>
int FileGDBOGRGeometryConverterImpl::ReadXYArray(XYSetter &setter,
                                                 GByte *&pabyCur,
                                                 GByte *pabyEnd,
                                                 GUInt32 nPoints, GIntBig &dx,
                                                 GIntBig &dy)
{
    const int errorRetValue = FALSE;
    GIntBig dxLocal = dx;
    GIntBig dyLocal = dy;

    for (GUInt32 i = 0; i < nPoints; i++)
    {
        returnErrorIf(pabyCur /*+ 1*/ >= pabyEnd);

        ReadVarIntAndAddNoCheck(pabyCur, dxLocal);
        ReadVarIntAndAddNoCheck(pabyCur, dyLocal);

        double dfX =
            dxLocal / poGeomField->GetXYScale() + poGeomField->GetXOrigin();
        double dfY =
            dyLocal / poGeomField->GetXYScale() + poGeomField->GetYOrigin();
        setter.set(i, dfX, dfY);
    }

    dx = dxLocal;
    dy = dyLocal;
    return TRUE;
}

/************************************************************************/
/*                          ZLineStringSetter                           */
/************************************************************************/

class ZLineStringSetter
{
    OGRLineString *poLS;

  public:
    explicit ZLineStringSetter(OGRLineString *poLSIn) : poLS(poLSIn)
    {
    }

    void set(int i, double dfZ)
    {
        poLS->setZ(i, dfZ);
    }
};

/************************************************************************/
/*                         ZMultiPointSetter                           */
/************************************************************************/

class ZMultiPointSetter
{
    OGRMultiPoint *poMPoint;

  public:
    explicit ZMultiPointSetter(OGRMultiPoint *poMPointIn) : poMPoint(poMPointIn)
    {
    }

    void set(int i, double dfZ)
    {
        poMPoint->getGeometryRef(i)->setZ(dfZ);
    }
};

/************************************************************************/
/*                             FileGDBArraySetter                            */
/************************************************************************/

class FileGDBArraySetter
{
    double *padfValues;

  public:
    explicit FileGDBArraySetter(double *padfValuesIn) : padfValues(padfValuesIn)
    {
    }

    void set(int i, double dfValue)
    {
        padfValues[i] = dfValue;
    }
};

/************************************************************************/
/*                          ReadZArray()                                */
/************************************************************************/

template <class ZSetter>
int FileGDBOGRGeometryConverterImpl::ReadZArray(ZSetter &setter,
                                                GByte *&pabyCur, GByte *pabyEnd,
                                                GUInt32 nPoints, GIntBig &dz)
{
    const int errorRetValue = FALSE;
    const double dfZScale = SanitizeScale(poGeomField->GetZScale());
    for (GUInt32 i = 0; i < nPoints; i++)
    {
        returnErrorIf(pabyCur >= pabyEnd);
        ReadVarIntAndAddNoCheck(pabyCur, dz);

        double dfZ = dz / dfZScale + poGeomField->GetZOrigin();
        setter.set(i, dfZ);
    }
    return TRUE;
}

/************************************************************************/
/*                          MLineStringSetter                           */
/************************************************************************/

class MLineStringSetter
{
    OGRLineString *poLS;

  public:
    explicit MLineStringSetter(OGRLineString *poLSIn) : poLS(poLSIn)
    {
    }

    void set(int i, double dfM)
    {
        poLS->setM(i, dfM);
    }
};

/************************************************************************/
/*                         MMultiPointSetter                           */
/************************************************************************/

class MMultiPointSetter
{
    OGRMultiPoint *poMPoint;

  public:
    explicit MMultiPointSetter(OGRMultiPoint *poMPointIn) : poMPoint(poMPointIn)
    {
    }

    void set(int i, double dfM)
    {
        poMPoint->getGeometryRef(i)->setM(dfM);
    }
};

/************************************************************************/
/*                          ReadMArray()                                */
/************************************************************************/

template <class MSetter>
int FileGDBOGRGeometryConverterImpl::ReadMArray(MSetter &setter,
                                                GByte *&pabyCur, GByte *pabyEnd,
                                                GUInt32 nPoints, GIntBig &dm)
{
    const int errorRetValue = FALSE;
    const double dfMScale = SanitizeScale(poGeomField->GetMScale());
    for (GUInt32 i = 0; i < nPoints; i++)
    {
        returnErrorIf(pabyCur >= pabyEnd);
        ReadVarIntAndAddNoCheck(pabyCur, dm);

        double dfM = dm / dfMScale + poGeomField->GetMOrigin();
        setter.set(i, dfM);
    }
    return TRUE;
}

/************************************************************************/
/*                          CreateCurveGeometry()                       */
/************************************************************************/

class XYBufferSetter
{
    GByte *pabyBuffer;

  public:
    explicit XYBufferSetter(GByte *pabyBufferIn) : pabyBuffer(pabyBufferIn)
    {
    }

    void set(int i, double dfX, double dfY)
    {
        CPL_LSBPTR64(&dfX);
        memcpy(pabyBuffer + 16 * i, &dfX, 8);
        CPL_LSBPTR64(&dfY);
        memcpy(pabyBuffer + 16 * i + 8, &dfY, 8);
    }
};

class ZOrMBufferSetter
{
    GByte *pabyBuffer;

  public:
    explicit ZOrMBufferSetter(GByte *pabyBufferIn) : pabyBuffer(pabyBufferIn)
    {
    }

    void set(int i, double dfValue)
    {
        CPL_LSBPTR64(&dfValue);
        memcpy(pabyBuffer + 8 * i, &dfValue, 8);
    }
};

/* We first create an extended shape buffer from the compressed stream */
/* and finally use OGRCreateFromShapeBin() to make a geometry from it */

OGRGeometry *FileGDBOGRGeometryConverterImpl::CreateCurveGeometry(
    GUInt32 nBaseShapeType, GUInt32 nParts, GUInt32 nPoints, GUInt32 nCurves,
    bool bHasZ, bool bHasM, GByte *&pabyCur, GByte *pabyEnd)
{
    OGRGeometry *errorRetValue = nullptr;
    GUInt32 i;
    const int nDims = 2 + (bHasZ ? 1 : 0) + (bHasM ? 1 : 0);
    GIntBig nMaxSize64 = 44 + 4 * static_cast<GUIntBig>(nParts) +
                         8 * nDims * static_cast<GUIntBig>(nPoints);
    nMaxSize64 += 4;  // nCurves
    nMaxSize64 +=
        static_cast<GUIntBig>(nCurves) * (4 + /* start index */
                                          4 + /* curve type */
                                          44 /* size of ellipse struct */);
    nMaxSize64 +=
        ((bHasZ ? 1 : 0) + (bHasM ? 1 : 0)) * 16;  // space for bounding boxes
    if (nMaxSize64 >= INT_MAX)
    {
        returnError();
    }
    const int nMaxSize = static_cast<int>(nMaxSize64 & INT_MAX);
    // coverity[overflow_sink]
    GByte *pabyExtShapeBuffer =
        static_cast<GByte *>(VSI_MALLOC_VERBOSE(nMaxSize));
    if (pabyExtShapeBuffer == nullptr)
    {
        VSIFree(pabyExtShapeBuffer);
        returnError();
    }
    GUInt32 nShapeType = nBaseShapeType | EXT_SHAPE_CURVE_FLAG;
    if (bHasZ)
        nShapeType |= EXT_SHAPE_Z_FLAG;
    if (bHasM)
        nShapeType |= EXT_SHAPE_M_FLAG;
    GUInt32 nTmp;
    nTmp = CPL_LSBWORD32(nShapeType);
    GByte *pabyShapeTypePtr = pabyExtShapeBuffer;
    memcpy(pabyExtShapeBuffer, &nTmp, 4);
    memset(pabyExtShapeBuffer + 4, 0, 32); /* bbox: unused */
    nTmp = CPL_LSBWORD32(nParts);
    memcpy(pabyExtShapeBuffer + 36, &nTmp, 4);
    nTmp = CPL_LSBWORD32(nPoints);
    memcpy(pabyExtShapeBuffer + 40, &nTmp, 4);
    GUInt32 nIdx = 0;
    for (i = 0; i < nParts; i++)
    {
        nTmp = CPL_LSBWORD32(nIdx);
        nIdx += panPointCount[i];
        memcpy(pabyExtShapeBuffer + 44 + 4 * i, &nTmp, 4);
    }
    int nOffset = 44 + 4 * nParts;
    GIntBig dx = 0;
    GIntBig dy = 0;
    XYBufferSetter arraySetter(pabyExtShapeBuffer + nOffset);
    if (!ReadXYArray<XYBufferSetter>(arraySetter, pabyCur, pabyEnd, nPoints, dx,
                                     dy))
    {
        VSIFree(pabyExtShapeBuffer);
        returnError();
    }
    nOffset += 16 * nPoints;

    if (bHasZ)
    {
        memset(pabyExtShapeBuffer + nOffset, 0, 16); /* bbox: unused */
        nOffset += 16;
        GIntBig dz = 0;
        ZOrMBufferSetter arrayzSetter(pabyExtShapeBuffer + nOffset);
        if (!ReadZArray<ZOrMBufferSetter>(arrayzSetter, pabyCur, pabyEnd,
                                          nPoints, dz))
        {
            VSIFree(pabyExtShapeBuffer);
            returnError();
        }
        nOffset += 8 * nPoints;
    }

    if (bHasM)
    {
        // It seems that absence of M is marked with a single byte
        // with value 66.
        if (*pabyCur == 66)
        {
            pabyCur++;
#if 1
            // In other code paths of this file, we drop the M component when
            // it is at null. The disabled code path would fill it with NaN
            // instead.
            nShapeType &= ~EXT_SHAPE_M_FLAG;
            nTmp = CPL_LSBWORD32(nShapeType);
            memcpy(pabyShapeTypePtr, &nTmp, 4);
#else
            memset(pabyExtShapeBuffer + nOffset, 0, 16); /* bbox: unused */
            nOffset += 16;
            const double myNan = std::numeric_limits<double>::quiet_NaN();
            for (i = 0; i < nPoints; i++)
            {
                memcpy(pabyExtShapeBuffer + nOffset + 8 * i, &myNan, 8);
                CPL_LSBPTR64(pabyExtShapeBuffer + nOffset + 8 * i);
            }
            nOffset += 8 * nPoints;
#endif
        }
        else
        {
            memset(pabyExtShapeBuffer + nOffset, 0, 16); /* bbox: unused */
            nOffset += 16;
            ZOrMBufferSetter arraymSetter(pabyExtShapeBuffer + nOffset);
            GIntBig dm = 0;
            if (!ReadMArray<ZOrMBufferSetter>(arraymSetter, pabyCur, pabyEnd,
                                              nPoints, dm))
            {
                VSIFree(pabyExtShapeBuffer);
                returnError();
            }
            nOffset += 8 * nPoints;
        }
    }

    nTmp = CPL_LSBWORD32(nCurves);
    memcpy(pabyExtShapeBuffer + nOffset, &nTmp, 4);
    nOffset += 4;
    for (i = 0; i < nCurves; i++)
    {
        // start index
        returnErrorAndCleanupIf(!ReadVarUInt32(pabyCur, pabyEnd, nTmp),
                                VSIFree(pabyExtShapeBuffer));
        CPL_LSBPTR32(&nTmp);
        memcpy(pabyExtShapeBuffer + nOffset, &nTmp, 4);
        nOffset += 4;

        GUInt32 nCurveType;
        returnErrorAndCleanupIf(!ReadVarUInt32(pabyCur, pabyEnd, nCurveType),
                                VSIFree(pabyExtShapeBuffer));
        nTmp = CPL_LSBWORD32(nCurveType);
        memcpy(pabyExtShapeBuffer + nOffset, &nTmp, 4);
        nOffset += 4;

        int nStructureSize = 0;
        if (nCurveType == EXT_SHAPE_SEGMENT_ARC)
            nStructureSize = 2 * 8 + 4;
        else if (nCurveType == EXT_SHAPE_SEGMENT_BEZIER)
            nStructureSize = 4 * 8;
        else if (nCurveType == EXT_SHAPE_SEGMENT_ELLIPSE)
            nStructureSize = 5 * 8 + 4;
        if (nStructureSize == 0 || pabyCur + nStructureSize > pabyEnd)
        {
            VSIFree(pabyExtShapeBuffer);
            returnError();
        }
        memcpy(pabyExtShapeBuffer + nOffset, pabyCur, nStructureSize);
        pabyCur += nStructureSize;
        nOffset += nStructureSize;
    }
    CPLAssert(nOffset <= nMaxSize);

    OGRGeometry *poRet = nullptr;
    OGRCreateFromShapeBin(pabyExtShapeBuffer, &poRet, nOffset);
    VSIFree(pabyExtShapeBuffer);
    return poRet;
}

/************************************************************************/
/*                          GetAsGeometry()                             */
/************************************************************************/

OGRGeometry *
FileGDBOGRGeometryConverterImpl::GetAsGeometry(const OGRField *psField)
{
    OGRGeometry *errorRetValue = nullptr;
    GByte *pabyCur = psField->Binary.paData;
    GByte *pabyEnd = pabyCur + psField->Binary.nCount;
    GUInt32 nGeomType, i, nPoints, nParts, nCurves;
    GUIntBig x, y, z;
    GIntBig dx, dy, dz;

    ReadVarUInt32NoCheck(pabyCur, nGeomType);

    bool bHasZ = (nGeomType & EXT_SHAPE_Z_FLAG) != 0;
    bool bHasM = (nGeomType & EXT_SHAPE_M_FLAG) != 0;
    switch ((nGeomType & 0xff))
    {
        case SHPT_NULL:
            return nullptr;

        case SHPT_POINTZ:
        case SHPT_POINTZM:
            bHasZ = true; /* go on */
            [[fallthrough]];
        case SHPT_POINT:
        case SHPT_POINTM:
        case SHPT_GENERALPOINT:
        {
            if (nGeomType == SHPT_POINTM || nGeomType == SHPT_POINTZM)
                bHasM = true;

            ReadVarUInt64NoCheck(pabyCur, x);
            ReadVarUInt64NoCheck(pabyCur, y);

            const double dfX = x == 0 ? std::numeric_limits<double>::quiet_NaN()
                                      : (x - 1U) / poGeomField->GetXYScale() +
                                            poGeomField->GetXOrigin();
            const double dfY = y == 0 ? std::numeric_limits<double>::quiet_NaN()
                                      : (y - 1U) / poGeomField->GetXYScale() +
                                            poGeomField->GetYOrigin();
            if (bHasZ)
            {
                ReadVarUInt64NoCheck(pabyCur, z);
                const double dfZScale = SanitizeScale(poGeomField->GetZScale());
                const double dfZ =
                    z == 0 ? std::numeric_limits<double>::quiet_NaN()
                           : (z - 1U) / dfZScale + poGeomField->GetZOrigin();
                if (bHasM)
                {
                    GUIntBig m = 0;
                    ReadVarUInt64NoCheck(pabyCur, m);
                    const double dfMScale =
                        SanitizeScale(poGeomField->GetMScale());
                    if (m == 0)
                    {
                        return new OGRPoint(
                            dfX, dfY, dfZ,
                            std::numeric_limits<double>::quiet_NaN());
                    }
                    else
                    {
                        assert(m >= 1U);
                        const double dfM =
                            (m - 1U) / dfMScale + poGeomField->GetMOrigin();
                        return new OGRPoint(dfX, dfY, dfZ, dfM);
                    }
                }
                return new OGRPoint(dfX, dfY, dfZ);
            }
            else if (bHasM)
            {
                OGRPoint *poPoint = new OGRPoint(dfX, dfY);
                GUIntBig m = 0;
                ReadVarUInt64NoCheck(pabyCur, m);
                const double dfMScale = SanitizeScale(poGeomField->GetMScale());
                const double dfM =
                    m == 0 ? std::numeric_limits<double>::quiet_NaN()
                           : (m - 1U) / dfMScale + poGeomField->GetMOrigin();
                poPoint->setM(dfM);
                return poPoint;
            }
            else
            {
                return new OGRPoint(dfX, dfY);
            }
            break;
        }

        case SHPT_MULTIPOINTZM:
        case SHPT_MULTIPOINTZ:
            bHasZ = true; /* go on */
            [[fallthrough]];
        case SHPT_MULTIPOINT:
        case SHPT_MULTIPOINTM:
        {
            if (nGeomType == SHPT_MULTIPOINTM || nGeomType == SHPT_MULTIPOINTZM)
                bHasM = true;

            returnErrorIf(!ReadVarUInt32(pabyCur, pabyEnd, nPoints));
            if (nPoints == 0)
            {
                OGRMultiPoint *poMP = new OGRMultiPoint();
                if (bHasZ)
                    poMP->set3D(TRUE);
                if (bHasM)
                    poMP->setMeasured(TRUE);
                return poMP;
            }

            returnErrorIf(!SkipVarUInt(pabyCur, pabyEnd, 4));

            dx = dy = dz = 0;

            OGRMultiPoint *poMP = new OGRMultiPoint();
            XYMultiPointSetter mpSetter(poMP);
            if (!ReadXYArray<XYMultiPointSetter>(mpSetter, pabyCur, pabyEnd,
                                                 nPoints, dx, dy))
            {
                delete poMP;
                returnError();
            }

            if (bHasZ)
            {
                poMP->setCoordinateDimension(3);
                ZMultiPointSetter mpzSetter(poMP);
                if (!ReadZArray<ZMultiPointSetter>(mpzSetter, pabyCur, pabyEnd,
                                                   nPoints, dz))
                {
                    delete poMP;
                    returnError();
                }
            }

            // It seems that absence of M is marked with a single byte
            // with value 66. Be more tolerant and only try to parse the M
            // array is there are at least as many remaining bytes as
            // expected points
            if (bHasM && pabyCur + nPoints <= pabyEnd)
            {
                poMP->setMeasured(TRUE);
                GIntBig dm = 0;
                MMultiPointSetter mpmSetter(poMP);
                if (!ReadMArray<MMultiPointSetter>(mpmSetter, pabyCur, pabyEnd,
                                                   nPoints, dm))
                {
                    delete poMP;
                    returnError();
                }
            }

            return poMP;
            // cppcheck-suppress duplicateBreak
            break;
        }

        case SHPT_ARCZ:
        case SHPT_ARCZM:
            bHasZ = true; /* go on */
            [[fallthrough]];
        case SHPT_ARC:
        case SHPT_ARCM:
        case SHPT_GENERALPOLYLINE:
        {
            if (nGeomType == SHPT_ARCM || nGeomType == SHPT_ARCZM)
                bHasM = true;

            returnErrorIf(
                !ReadPartDefs(pabyCur, pabyEnd, nPoints, nParts, nCurves,
                              (nGeomType & EXT_SHAPE_CURVE_FLAG) != 0, false));

            if (nPoints == 0 || nParts == 0)
            {
                OGRLineString *poLS = new OGRLineString();
                if (bHasZ)
                    poLS->set3D(TRUE);
                if (bHasM)
                    poLS->setMeasured(TRUE);
                return poLS;
            }

            if (nCurves)
            {
                GByte *pabyCurBackup = pabyCur;
                OGRGeometry *poRet = CreateCurveGeometry(
                    SHPT_GENERALPOLYLINE, nParts, nPoints, nCurves, bHasZ,
                    bHasM, pabyCur, pabyEnd);
                if (poRet)
                    return poRet;
                // In case something went wrong, go on without curves
                pabyCur = pabyCurBackup;
            }

            OGRMultiLineString *poMLS = nullptr;
            FileGDBOGRLineString *poLS = nullptr;
            if (nParts > 1)
            {
                poMLS = new OGRMultiLineString();
                if (bHasZ)
                    poMLS->set3D(TRUE);
                if (bHasM)
                    poMLS->setMeasured(TRUE);
            }

            dx = dy = dz = 0;
            for (i = 0; i < nParts; i++)
            {
                poLS = new FileGDBOGRLineString();
                poLS->setNumPoints(panPointCount[i], FALSE);
                if (nParts > 1)
                    poMLS->addGeometryDirectly(poLS);

                XYLineStringSetter lsSetter(poLS->GetPoints());
                if (!ReadXYArray<XYLineStringSetter>(lsSetter, pabyCur, pabyEnd,
                                                     panPointCount[i], dx, dy))
                {
                    if (nParts > 1)
                        delete poMLS;
                    else
                        delete poLS;
                    returnError();
                }
            }

            if (bHasZ)
            {
                for (i = 0; i < nParts; i++)
                {
                    if (nParts > 1)
                        poLS = cpl::down_cast<FileGDBOGRLineString *>(
                            poMLS->getGeometryRef(i));

                    ZLineStringSetter lszSetter(poLS);
                    if (!ReadZArray<ZLineStringSetter>(
                            lszSetter, pabyCur, pabyEnd, panPointCount[i], dz))
                    {
                        if (nParts > 1)
                            delete poMLS;
                        else
                            delete poLS;
                        returnError();
                    }
                }
            }

            if (bHasM)
            {
                GIntBig dm = 0;
                for (i = 0; i < nParts; i++)
                {
                    if (nParts > 1)
                        poLS = cpl::down_cast<FileGDBOGRLineString *>(
                            poMLS->getGeometryRef(i));

                    // It seems that absence of M is marked with a single byte
                    // with value 66. Be more tolerant and only try to parse the
                    // M array is there are at least as many remaining bytes as
                    // expected points
                    if (pabyCur + panPointCount[i] > pabyEnd)
                    {
                        if (nParts > 1)
                            poMLS->setMeasured(FALSE);
                        break;
                    }

                    MLineStringSetter lsmSetter(poLS);
                    if (!ReadMArray<MLineStringSetter>(
                            lsmSetter, pabyCur, pabyEnd, panPointCount[i], dm))
                    {
                        if (nParts > 1)
                            delete poMLS;
                        else
                            delete poLS;
                        returnError();
                    }
                }
            }

            if (poMLS)
                return poMLS;
            else
                return poLS;

            break;
        }

        case SHPT_POLYGONZ:
        case SHPT_POLYGONZM:
            bHasZ = true; /* go on */
            [[fallthrough]];
        case SHPT_POLYGON:
        case SHPT_POLYGONM:
        case SHPT_GENERALPOLYGON:
        {
            if (nGeomType == SHPT_POLYGONM || nGeomType == SHPT_POLYGONZM)
                bHasM = true;

            returnErrorIf(
                !ReadPartDefs(pabyCur, pabyEnd, nPoints, nParts, nCurves,
                              (nGeomType & EXT_SHAPE_CURVE_FLAG) != 0, false));

            if (nPoints == 0 || nParts == 0)
            {
                OGRPolygon *poPoly = new OGRPolygon();
                if (bHasZ)
                    poPoly->set3D(TRUE);
                if (bHasM)
                    poPoly->setMeasured(TRUE);
                return poPoly;
            }

            if (nCurves)
            {
                GByte *pabyCurBackup = pabyCur;
                OGRGeometry *poRet = CreateCurveGeometry(
                    SHPT_GENERALPOLYGON, nParts, nPoints, nCurves, bHasZ, bHasM,
                    pabyCur, pabyEnd);
                if (poRet)
                    return poRet;
                // In case something went wrong, go on without curves
                pabyCur = pabyCurBackup;
            }

            OGRLinearRing **papoRings = new OGRLinearRing *[nParts];

            dx = dy = dz = 0;
            for (i = 0; i < nParts; i++)
            {
                FileGDBOGRLinearRing *poRing = new FileGDBOGRLinearRing();
                papoRings[i] = poRing;
                poRing->setNumPoints(panPointCount[i], FALSE);

                XYLineStringSetter lsSetter(poRing->GetPoints());
                if (!ReadXYArray<XYLineStringSetter>(lsSetter, pabyCur, pabyEnd,
                                                     panPointCount[i], dx, dy))
                {
                    while (true)
                    {
                        delete papoRings[i];
                        if (i == 0)
                            break;
                        i--;
                    }
                    delete[] papoRings;
                    // For some reason things that papoRings is leaking
                    // cppcheck-suppress memleak
                    returnError();
                }
            }

            if (bHasZ)
            {
                for (i = 0; i < nParts; i++)
                {
                    papoRings[i]->setCoordinateDimension(3);

                    ZLineStringSetter lszSetter(papoRings[i]);
                    if (!ReadZArray<ZLineStringSetter>(
                            lszSetter, pabyCur, pabyEnd, panPointCount[i], dz))
                    {
                        for (i = 0; i < nParts; i++)
                            delete papoRings[i];
                        delete[] papoRings;
                        returnError();
                    }
                }
            }

            if (bHasM)
            {
                GIntBig dm = 0;
                for (i = 0; i < nParts; i++)
                {
                    // It seems that absence of M is marked with a single byte
                    // with value 66. Be more tolerant and only try to parse the
                    // M array is there are at least as many remaining bytes as
                    // expected points
                    if (pabyCur + panPointCount[i] > pabyEnd)
                    {
                        while (i != 0)
                        {
                            --i;
                            papoRings[i]->setMeasured(FALSE);
                        }
                        break;
                    }

                    papoRings[i]->setMeasured(TRUE);

                    MLineStringSetter lsmSetter(papoRings[i]);
                    if (!ReadMArray<MLineStringSetter>(
                            lsmSetter, pabyCur, pabyEnd, panPointCount[i], dm))
                    {
                        for (i = 0; i < nParts; i++)
                            delete papoRings[i];
                        delete[] papoRings;
                        returnError();
                    }
                }
            }

            OGRGeometry *poRet = nullptr;
            if (nParts == 1)
            {
                OGRPolygon *poPoly = new OGRPolygon();
                poRet = poPoly;
                poPoly->addRingDirectly(papoRings[0]);
            }
            else
            // Note: ASSUME_INNER_RINGS_IMMEDIATELY_AFTER_OUTER_RING is *not* defined
#ifdef ASSUME_INNER_RINGS_IMMEDIATELY_AFTER_OUTER_RING
                if (bUseOrganize || !(papoRings[0]->isClockwise()))
#endif
            {
                /* Slow method: we do a rather expensive topological analysis of
                 * the rings to figure out which ones are inner rings from outer
                 * rings, and to which outer ring an inner ring belongs too.
                 * In most cases, inner rings are CCW oriented and follow
                 * immediately the outer ring in which they are included,
                 * (that situation is the commented code in the below else
                 * branch).
                 * In nearly all cases, inner rings are CCW and outer rings
                 * are CW oriented, so we could call organizePolygons() with
                 * the relatively lightweight METHOD=ONLY_CCW strategy (which
                 * is what the shapefile drivers does at time of writing).
                 * Unfortunately in https://github.com/OSGeo/gdal/issues/1369,
                 * we found likely broken datasets where a polygon with inner
                 * rings has its exterior ring with wrong orientation, hence
                 * we use the slowest but bullet-proof method.
                 */
                OGRPolygon **papoPolygons = new OGRPolygon *[nParts];
                for (i = 0; i < nParts; i++)
                {
                    papoPolygons[i] = new OGRPolygon();
                    papoPolygons[i]->addRingDirectly(papoRings[i]);
                }
                delete[] papoRings;
                papoRings = nullptr;
                poRet = OGRGeometryFactory::organizePolygons(
                    reinterpret_cast<OGRGeometry **>(papoPolygons), nParts,
                    nullptr, nullptr);
                delete[] papoPolygons;
            }
#ifdef ASSUME_INNER_RINGS_IMMEDIATELY_AFTER_OUTER_RING
            else
            {
                /* Inner rings are CCW oriented and follow immediately the outer
                 */
                /* ring (that is CW oriented) in which they are included */
                OGRMultiPolygon *poMulti = NULL;
                OGRPolygon *poCur = new OGRPolygon();
                poRet = poCur;
                /* We have already checked that the first ring is CW */
                poCur->addRingDirectly(papoRings[0]);
                OGREnvelope sEnvelope;
                papoRings[0]->getEnvelope(&sEnvelope);
                for (i = 1; i < nParts; i++)
                {
                    int bIsCW = papoRings[i]->isClockwise();
                    if (bIsCW)
                    {
                        if (poMulti == NULL)
                        {
                            poMulti = new OGRMultiPolygon();
                            poRet = poMulti;
                            poMulti->addGeometryDirectly(poCur);
                        }
                        poCur = new OGRPolygon();
                        poMulti->addGeometryDirectly(poCur);
                        poCur->addRingDirectly(papoRings[i]);
                        papoRings[i]->getEnvelope(&sEnvelope);
                    }
                    else
                    {
                        poCur->addRingDirectly(papoRings[i]);
                        OGRPoint oPoint;
                        papoRings[i]->getPoint(0, &oPoint);
                        CPLAssert(oPoint.getX() >= sEnvelope.MinX &&
                                  oPoint.getX() <= sEnvelope.MaxX &&
                                  oPoint.getY() >= sEnvelope.MinY &&
                                  oPoint.getY() <= sEnvelope.MaxY);
                    }
                }
            }
#endif

            delete[] papoRings;
            return poRet;
            // cppcheck-suppress duplicateBreak
            break;
        }

        case SHPT_MULTIPATCHM:
        case SHPT_MULTIPATCH:
            bHasZ = true; /* go on */
            [[fallthrough]];
        case SHPT_GENERALMULTIPATCH:
        {
            returnErrorIf(!ReadPartDefs(pabyCur, pabyEnd, nPoints, nParts,
                                        nCurves, false, true));

            if (nPoints == 0 || nParts == 0)
            {
                OGRPolygon *poPoly = new OGRPolygon();
                if (bHasZ)
                    poPoly->setCoordinateDimension(3);
                return poPoly;
            }
            int *panPartType =
                static_cast<int *>(VSI_MALLOC_VERBOSE(sizeof(int) * nParts));
            // The + 1 is to add an extra element, not actually used, but
            // to please Coverity Scan
            int *panPartStart = static_cast<int *>(
                VSI_MALLOC_VERBOSE(sizeof(int) * (nParts + 1)));
            double *padfXYZ = static_cast<double *>(
                VSI_MALLOC_VERBOSE(3 * sizeof(double) * nPoints));
            double *padfX = padfXYZ;
            double *padfY = padfXYZ ? padfXYZ + nPoints : nullptr;
            double *padfZ = padfXYZ ? padfXYZ + 2 * nPoints : nullptr;
            if (panPartType == nullptr || panPartStart == nullptr ||
                padfXYZ == nullptr)
            {
                VSIFree(panPartType);
                VSIFree(panPartStart);
                VSIFree(padfXYZ);
                returnError();
            }
            for (i = 0; i < nParts; i++)
            {
                GUInt32 nPartType;
                if (!ReadVarUInt32(pabyCur, pabyEnd, nPartType))
                {
                    VSIFree(panPartType);
                    VSIFree(panPartStart);
                    VSIFree(padfXYZ);
                    returnError();
                }
                panPartType[i] = static_cast<int>(nPartType);
            }
            dx = dy = dz = 0;

            XYArraySetter arraySetter(padfX, padfY);
            if (!ReadXYArray<XYArraySetter>(arraySetter, pabyCur, pabyEnd,
                                            nPoints, dx, dy))
            {
                VSIFree(panPartType);
                VSIFree(panPartStart);
                VSIFree(padfXYZ);
                returnError();
            }

            if (bHasZ)
            {
                FileGDBArraySetter arrayzSetter(padfZ);
                if (!ReadZArray<FileGDBArraySetter>(arrayzSetter, pabyCur,
                                                    pabyEnd, nPoints, dz))
                {
                    VSIFree(panPartType);
                    VSIFree(panPartStart);
                    VSIFree(padfXYZ);
                    returnError();
                }
            }
            else
            {
                memset(padfZ, 0, nPoints * sizeof(double));
            }

            panPartStart[0] = 0;
            for (i = 1; i < nParts; ++i)
                panPartStart[i] = panPartStart[i - 1] + panPointCount[i - 1];
            // Not used, but avoids a Coverity Scan warning
            panPartStart[nParts] = nPoints;
            OGRGeometry *poRet = OGRCreateFromMultiPatch(
                static_cast<int>(nParts), panPartStart, panPartType,
                static_cast<int>(nPoints), padfX, padfY, padfZ);

            VSIFree(panPartType);
            VSIFree(panPartStart);
            VSIFree(padfXYZ);

            return poRet;
            // cppcheck-suppress duplicateBreak
            break;
        }

        default:
            CPLDebug("OpenFileGDB", "Unhandled geometry type = %d",
                     static_cast<int>(nGeomType));
            break;
            /*
            #define SHPT_GENERALMULTIPOINT  53
            */
    }
    return nullptr;
}

/************************************************************************/
/*                           BuildConverter()                           */
/************************************************************************/

FileGDBOGRGeometryConverter *
FileGDBOGRGeometryConverter::BuildConverter(const FileGDBGeomField *poGeomField)
{
    return new FileGDBOGRGeometryConverterImpl(poGeomField);
}

/************************************************************************/
/*                      GetGeometryTypeFromESRI()                       */
/************************************************************************/

static const struct
{
    const char *pszStr;
    OGRwkbGeometryType eType;
} AssocESRIGeomTypeToOGRGeomType[] = {
    {"esriGeometryPoint", wkbPoint},
    {"esriGeometryMultipoint", wkbMultiPoint},
    {"esriGeometryLine", wkbMultiLineString},
    {"esriGeometryPolyline", wkbMultiLineString},
    {"esriGeometryPolygon", wkbMultiPolygon},
    {"esriGeometryMultiPatch", wkbUnknown}};

OGRwkbGeometryType
FileGDBOGRGeometryConverter::GetGeometryTypeFromESRI(const char *pszESRIType)
{
    for (size_t i = 0; i < sizeof(AssocESRIGeomTypeToOGRGeomType) /
                               sizeof(AssocESRIGeomTypeToOGRGeomType[0]);
         i++)
    {
        if (strcmp(pszESRIType, AssocESRIGeomTypeToOGRGeomType[i].pszStr) == 0)
            return AssocESRIGeomTypeToOGRGeomType[i].eType;
    }
    CPLDebug("OpenFileGDB", "Unhandled geometry type : %s", pszESRIType);
    return wkbUnknown;
}

} /* namespace OpenFileGDB */
