/******************************************************************************
 *
 * Project:  Feather Translator
 * Purpose:  Implements OGRFeatherDriver.
 * Author:   Even Rouault, <even.rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2022, Planet Labs
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_json.h"
#include "cpl_time.h"
#include "gdal_pam.h"
#include "ogrsf_frmts.h"
#include "ogr_p.h"

#include <cinttypes>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "ogr_feather.h"

#include "../arrow_common/ograrrowlayer.hpp"
#include "../arrow_common/ograrrowdataset.hpp"

/************************************************************************/
/*                        OGRFeatherLayer()                             */
/************************************************************************/

OGRFeatherLayer::OGRFeatherLayer(
    OGRFeatherDataset *poDS, const char *pszLayerName,
    std::shared_ptr<arrow::ipc::RecordBatchFileReader> &poRecordBatchFileReader)
    : OGRArrowLayer(poDS, pszLayerName), m_poDS(poDS),
      m_poRecordBatchFileReader(poRecordBatchFileReader)
{
    EstablishFeatureDefn();
    CPLAssert(static_cast<int>(m_aeGeomEncoding.size()) ==
              m_poFeatureDefn->GetGeomFieldCount());
}

/************************************************************************/
/*                        OGRFeatherLayer()                             */
/************************************************************************/

OGRFeatherLayer::OGRFeatherLayer(
    OGRFeatherDataset *poDS, const char *pszLayerName,
    std::shared_ptr<arrow::io::RandomAccessFile> poFile, bool bSeekable,
    const arrow::ipc::IpcReadOptions &oOptions,
    std::shared_ptr<arrow::ipc::RecordBatchStreamReader>
        &poRecordBatchStreamReader)
    : OGRArrowLayer(poDS, pszLayerName), m_poDS(poDS),
      m_poFile(std::move(poFile)), m_bSeekable(bSeekable), m_oOptions(oOptions),
      m_poRecordBatchReader(poRecordBatchStreamReader)
{
    EstablishFeatureDefn();
    CPLAssert(static_cast<int>(m_aeGeomEncoding.size()) ==
              m_poFeatureDefn->GetGeomFieldCount());
}

/************************************************************************/
/*                           GetDataset()                               */
/************************************************************************/

GDALDataset *OGRFeatherLayer::GetDataset()
{
    return m_poDS;
}

/************************************************************************/
/*                          LoadGeoMetadata()                           */
/************************************************************************/

void OGRFeatherLayer::LoadGeoMetadata(
    const arrow::KeyValueMetadata *kv_metadata, const std::string &key)
{
    if (kv_metadata && kv_metadata->Contains(key))
    {
        auto geo = kv_metadata->Get(key);
        if (geo.ok())
        {
            CPLJSONDocument oDoc;
            if (oDoc.LoadMemory(*geo))
            {
                auto oRoot = oDoc.GetRoot();
                const auto osVersion = oRoot.GetString("schema_version");
                if (key != GDAL_GEO_FOOTER_KEY && osVersion != "0.1.0")
                {
                    CPLDebug("FEATHER",
                             "schema_version = %s not explicitly handled by "
                             "the driver",
                             osVersion.c_str());
                }
                auto oColumns = oRoot.GetObj("columns");
                if (oColumns.IsValid())
                {
                    for (const auto &oColumn : oColumns.GetChildren())
                    {
                        m_oMapGeometryColumns[oColumn.GetName()] = oColumn;
                    }
                }
            }
            else
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Cannot parse 'geo' metadata");
            }
        }
    }
}

/************************************************************************/
/*                        EstablishFeatureDefn()                        */
/************************************************************************/

void OGRFeatherLayer::EstablishFeatureDefn()
{
    m_poSchema = m_poRecordBatchFileReader ? m_poRecordBatchFileReader->schema()
                                           : m_poRecordBatchReader->schema();
    const auto &kv_metadata = m_poSchema->metadata();

#ifdef DEBUG
    if (kv_metadata)
    {
        for (const auto &keyValue : kv_metadata->sorted_pairs())
        {
            CPLDebug("FEATHER", "%s = %s", keyValue.first.c_str(),
                     keyValue.second.c_str());
        }
    }
#endif

    auto poFooterMetadata = m_poRecordBatchFileReader
                                ? m_poRecordBatchFileReader->metadata()
                                : nullptr;
    if (poFooterMetadata && poFooterMetadata->Contains(GDAL_GEO_FOOTER_KEY) &&
        CPLTestBool(CPLGetConfigOption("OGR_ARROW_READ_GDAL_FOOTER", "YES")))
    {
        LoadGeoMetadata(poFooterMetadata.get(), GDAL_GEO_FOOTER_KEY);
    }
    else
    {
        LoadGeoMetadata(kv_metadata.get(), "geo");
    }
    const auto oMapFieldNameToGDALSchemaFieldDefn =
        LoadGDALSchema(kv_metadata.get());

    const auto &fields = m_poSchema->fields();
    for (int i = 0; i < m_poSchema->num_fields(); ++i)
    {
        const auto &field = fields[i];
        const auto &fieldName = field->name();

        const auto &field_kv_metadata = field->metadata();
        std::string osExtensionName;
        std::string osExtensionMetadata;
        if (field->type()->id() == arrow::Type::EXTENSION)
        {
            osExtensionName =
                cpl::down_cast<arrow::ExtensionType *>(field->type().get())
                    ->extension_name();
        }
        else if (field_kv_metadata)
        {
            auto extension_name =
                field_kv_metadata->Get(ARROW_EXTENSION_NAME_KEY);
            if (extension_name.ok())
            {
                osExtensionName = *extension_name;
            }

            auto extension_metadata =
                field_kv_metadata->Get(ARROW_EXTENSION_METADATA_KEY);
            if (extension_metadata.ok())
            {
                osExtensionMetadata = *extension_metadata;
            }
#ifdef DEBUG
            CPLDebug("FEATHER", "Metadata field %s:", fieldName.c_str());
            for (const auto &keyValue : field_kv_metadata->sorted_pairs())
            {
                CPLDebug("FEATHER", "  %s = %s", keyValue.first.c_str(),
                         keyValue.second.c_str());
            }
#endif
        }

        if (!m_osFIDColumn.empty() && fieldName == m_osFIDColumn)
        {
            m_iFIDArrowColumn = i;
            continue;
        }

        bool bRegularField = true;
        auto oIter = m_oMapGeometryColumns.find(fieldName);
        if (oIter != m_oMapGeometryColumns.end() || !osExtensionName.empty())
        {
            CPLJSONObject oJSONDef;
            if (oIter != m_oMapGeometryColumns.end())
                oJSONDef = oIter->second;
            auto osEncoding = oJSONDef.GetString("encoding");
            if (osEncoding.empty() && !osExtensionName.empty())
                osEncoding = osExtensionName;

            OGRwkbGeometryType eGeomType = wkbUnknown;
            auto eGeomEncoding = OGRArrowGeomEncoding::WKB;
            if (IsValidGeometryEncoding(field, osEncoding,
                                        oIter != m_oMapGeometryColumns.end(),
                                        eGeomType, eGeomEncoding))
            {
                bRegularField = false;
                OGRGeomFieldDefn oField(fieldName.c_str(), wkbUnknown);

                auto osCRS = oJSONDef.GetString("crs");

#if ARROW_VERSION_MAJOR >= 21
                if (osExtensionName == EXTENSION_NAME_GEOARROW_WKB &&
                    osExtensionMetadata.empty() &&
                    field->type()->id() == arrow::Type::EXTENSION)
                {
                    const auto arrowWkb =
                        std::dynamic_pointer_cast<OGRGeoArrowWkbExtensionType>(
                            field->type());
                    if (arrowWkb)
                    {
                        osExtensionMetadata = arrowWkb->Serialize();
                    }
                }
#endif

                if (osCRS.empty() &&
                    osExtensionName == EXTENSION_NAME_GEOARROW_WKB &&
                    !osExtensionMetadata.empty() &&
                    osExtensionMetadata[0] == '{' &&
                    osExtensionMetadata.back() == '}')
                {
                    CPLJSONDocument oDoc;
                    if (oDoc.LoadMemory(osExtensionMetadata))
                    {
                        auto jCrs = oDoc.GetRoot()["crs"];
                        if (jCrs.GetType() == CPLJSONObject::Type::Object)
                        {
                            osCRS =
                                jCrs.Format(CPLJSONObject::PrettyFormat::Plain);
                        }
                        else if (jCrs.GetType() == CPLJSONObject::Type::String)
                        {
                            osCRS = jCrs.ToString();
                        }
                        if (oDoc.GetRoot()["edges"].ToString() == "spherical")
                        {
                            SetMetadataItem("EDGES", "SPHERICAL");
                        }
                    }
                }

                if (osCRS.empty())
                {
#if 0
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Missing required 'crs' field for geometry column %s",
                             fieldName.c_str());
#endif
                }
                else
                {
                    OGRSpatialReference *poSRS = new OGRSpatialReference();
                    poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

                    if (poSRS->SetFromUserInput(
                            osCRS.c_str(),
                            OGRSpatialReference::
                                SET_FROM_USER_INPUT_LIMITATIONS_get()) ==
                        OGRERR_NONE)
                    {
                        const char *pszAuthName =
                            poSRS->GetAuthorityName(nullptr);
                        const char *pszAuthCode =
                            poSRS->GetAuthorityCode(nullptr);
                        if (pszAuthName && pszAuthCode &&
                            EQUAL(pszAuthName, "OGC") &&
                            EQUAL(pszAuthCode, "CRS84"))
                        {
                            poSRS->importFromEPSG(4326);
                        }

                        const double dfCoordEpoch = oJSONDef.GetDouble("epoch");
                        if (dfCoordEpoch > 0)
                            poSRS->SetCoordinateEpoch(dfCoordEpoch);

                        oField.SetSpatialRef(poSRS);
                    }
                    poSRS->Release();
                }

                // m_aeGeomEncoding be filled before calling
                // ComputeGeometryColumnType()
                m_aeGeomEncoding.push_back(eGeomEncoding);
                if (eGeomType == wkbUnknown)
                {
                    auto osType = oJSONDef.GetString("geometry_type");
                    if (osType.empty())
                        osType = oJSONDef.GetString("gdal:geometry_type");
                    if (m_bSeekable && osType.empty() &&
                        CPLTestBool(CPLGetConfigOption(
                            "OGR_ARROW_COMPUTE_GEOMETRY_TYPE", "YES")))
                    {
                        eGeomType = ComputeGeometryColumnType(
                            m_poFeatureDefn->GetGeomFieldCount(), i);
                        if (m_poRecordBatchReader)
                            ResetRecordBatchReader();
                    }
                    else
                        eGeomType = GetGeometryTypeFromString(osType);
                }

                oField.SetType(eGeomType);
                oField.SetNullable(field->nullable());
                m_poFeatureDefn->AddGeomFieldDefn(&oField);
                m_anMapGeomFieldIndexToArrowColumn.push_back(i);
            }
        }

        if (bRegularField)
        {
            CreateFieldFromSchema(field, {i},
                                  oMapFieldNameToGDALSchemaFieldDefn);
        }
    }

    CPLAssert(static_cast<int>(m_anMapFieldIndexToArrowColumn.size()) ==
              m_poFeatureDefn->GetFieldCount());
    CPLAssert(static_cast<int>(m_anMapGeomFieldIndexToArrowColumn.size()) ==
              m_poFeatureDefn->GetGeomFieldCount());
}

/************************************************************************/
/*                       ResetRecordBatchReader()                       */
/************************************************************************/

bool OGRFeatherLayer::ResetRecordBatchReader()
{
    const auto nPos = *(m_poFile->Tell());
    CPL_IGNORE_RET_VAL(m_poFile->Seek(0));
    auto result =
        arrow::ipc::RecordBatchStreamReader::Open(m_poFile, m_oOptions);
    if (!result.ok())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "RecordBatchStreamReader::Open() failed with %s",
                 result.status().message().c_str());
        CPL_IGNORE_RET_VAL(m_poFile->Seek(nPos));
        return false;
    }
    else
    {
        m_poRecordBatchReader = *result;
        return true;
    }
}

/************************************************************************/
/*                     ComputeGeometryColumnType()                      */
/************************************************************************/

OGRwkbGeometryType OGRFeatherLayer::ComputeGeometryColumnType(int iGeomCol,
                                                              int iCol) const
{
    // Compute type of geometry column by iterating over each geometry, and
    // looking at the WKB geometry type in the first 5 bytes of each geometry.

    OGRwkbGeometryType eGeomType = wkbNone;

    if (m_poRecordBatchReader != nullptr)
    {
        std::shared_ptr<arrow::RecordBatch> poBatch;
        while (true)
        {
            auto status = m_poRecordBatchReader->ReadNext(&poBatch);
            if (!status.ok())
            {
                CPLError(CE_Failure, CPLE_AppDefined, "ReadNext() failed: %s",
                         status.message().c_str());
                break;
            }
            else if (!poBatch)
                break;
            eGeomType = ComputeGeometryColumnTypeProcessBatch(poBatch, iGeomCol,
                                                              iCol, eGeomType);
            if (eGeomType == wkbUnknown)
                break;
        }
    }
    else
    {
        for (int iBatch = 0;
             iBatch < m_poRecordBatchFileReader->num_record_batches(); ++iBatch)
        {
            auto result = m_poRecordBatchFileReader->ReadRecordBatch(iBatch);
            if (!result.ok())
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "ReadRecordBatch() failed: %s",
                         result.status().message().c_str());
                break;
            }
            eGeomType = ComputeGeometryColumnTypeProcessBatch(*result, iGeomCol,
                                                              iCol, eGeomType);
            if (eGeomType == wkbUnknown)
                break;
        }
    }

    return eGeomType == wkbNone ? wkbUnknown : eGeomType;
}

/************************************************************************/
/*                          BuildDomain()                               */
/************************************************************************/

std::unique_ptr<OGRFieldDomain>
OGRFeatherLayer::BuildDomain(const std::string &osDomainName,
                             int iFieldIndex) const
{
    const int iArrowCol = m_anMapFieldIndexToArrowColumn[iFieldIndex][0];
    CPLAssert(m_poSchema->fields()[iArrowCol]->type()->id() ==
              arrow::Type::DICTIONARY);

    if (m_poRecordBatchReader)
    {
        if (m_poBatch)
        {
            return BuildDomainFromBatch(osDomainName, m_poBatch, iArrowCol);
        }
    }
    else if (m_poRecordBatchFileReader)
    {
        auto result = m_poRecordBatchFileReader->ReadRecordBatch(0);
        if (!result.ok())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "ReadRecordBatch() failed: %s",
                     result.status().message().c_str());
        }
        auto poBatch = *result;
        if (poBatch)
        {
            return BuildDomainFromBatch(osDomainName, poBatch, iArrowCol);
        }
    }

    return nullptr;
}

/************************************************************************/
/*                           ResetReading()                             */
/************************************************************************/

void OGRFeatherLayer::ResetReading()
{
    if (m_poRecordBatchReader != nullptr && m_iRecordBatch > 0)
    {
        if (m_iRecordBatch == 1 && m_poBatchIdx1)
        {
            // do nothing
        }
        else
        {
            m_bResetRecordBatchReaderAsked = true;
        }
    }
    OGRArrowLayer::ResetReading();
}

/************************************************************************/
/*                           ReadNextBatch()                            */
/************************************************************************/

bool OGRFeatherLayer::ReadNextBatch()
{
    if (m_poRecordBatchFileReader == nullptr)
    {
        return ReadNextBatchStream();
    }
    else
    {
        return ReadNextBatchFile();
    }
}

/************************************************************************/
/*                         ReadNextBatchFile()                          */
/************************************************************************/

bool OGRFeatherLayer::ReadNextBatchFile()
{
    while (true)
    {
        ++m_iRecordBatch;
        if (m_iRecordBatch == m_poRecordBatchFileReader->num_record_batches())
        {
            if (m_iRecordBatch == 1)
                m_iRecordBatch = 0;
            else
                m_poBatch.reset();
            return false;
        }

        m_nIdxInBatch = 0;

        auto result =
            m_poRecordBatchFileReader->ReadRecordBatch(m_iRecordBatch);
        if (!result.ok())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "ReadRecordBatch() failed: %s",
                     result.status().message().c_str());
            m_poBatch.reset();
            return false;
        }
        if ((*result)->num_rows() != 0)
        {
            SetBatch(*result);
            break;
        }
    }

    return true;
}

/************************************************************************/
/*                         ReadNextBatchStream()                        */
/************************************************************************/

bool OGRFeatherLayer::ReadNextBatchStream()
{
    m_nIdxInBatch = 0;

    std::shared_ptr<arrow::RecordBatch> poNextBatch;
    do
    {
        if (m_iRecordBatch == 0 && m_poBatchIdx0)
        {
            SetBatch(m_poBatchIdx0);
            m_iRecordBatch = 1;
            return true;
        }

        else if (m_iRecordBatch == 1 && m_poBatchIdx1)
        {
            SetBatch(m_poBatchIdx1);
            m_iRecordBatch = 2;
            return true;
        }

        else if (m_bSingleBatch)
        {
            CPLAssert(m_iRecordBatch == 0);
            CPLAssert(m_poBatch != nullptr);
            return false;
        }

        if (m_bResetRecordBatchReaderAsked)
        {
            if (!m_bSeekable)
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "Attempting to rewind non-seekable stream");
                return false;
            }
            if (!ResetRecordBatchReader())
                return false;
            m_bResetRecordBatchReaderAsked = false;
        }

        CPLAssert(m_poRecordBatchReader);

        ++m_iRecordBatch;

        poNextBatch.reset();
        auto status = m_poRecordBatchReader->ReadNext(&poNextBatch);
        if (!status.ok())
        {
            CPLError(CE_Failure, CPLE_AppDefined, "ReadNext() failed: %s",
                     status.message().c_str());
            poNextBatch.reset();
        }
        if (poNextBatch == nullptr)
        {
            if (m_iRecordBatch == 1)
            {
                m_iRecordBatch = 0;
                m_bSingleBatch = true;
            }
            else
            {
                m_poBatch.reset();
                m_poBatchColumns.clear();
            }
            return false;
        }
    } while (poNextBatch->num_rows() == 0);

    SetBatch(poNextBatch);

    return true;
}

/************************************************************************/
/*                     TryToCacheFirstTwoBatches()                      */
/************************************************************************/

void OGRFeatherLayer::TryToCacheFirstTwoBatches()
{
    if (m_poRecordBatchReader != nullptr && m_iRecordBatch <= 0 &&
        !m_bSingleBatch && m_poBatchIdx0 == nullptr)
    {
        ResetReading();
        if (!m_poBatch)
        {
            CPL_IGNORE_RET_VAL(ReadNextBatchStream());
        }
        if (m_poBatch)
        {
            auto poBatchIdx0 = m_poBatch;
            if (ReadNextBatchStream())
            {
                CPLAssert(m_iRecordBatch == 1);
                m_poBatchIdx0 = poBatchIdx0;
                m_poBatchIdx1 = m_poBatch;
                SetBatch(poBatchIdx0);
                ResetReading();
            }
            ResetReading();
        }
    }
}

/************************************************************************/
/*                          CanPostFilterArrowArray()                   */
/************************************************************************/

bool OGRFeatherLayer::CanPostFilterArrowArray(
    const struct ArrowSchema *schema) const
{
    if (m_poRecordBatchReader)
        return false;
    return OGRArrowLayer::CanPostFilterArrowArray(schema);
}

/************************************************************************/
/*                     InvalidateCachedBatches()                        */
/************************************************************************/

void OGRFeatherLayer::InvalidateCachedBatches()
{
    if (m_poRecordBatchFileReader)
    {
        m_iRecordBatch = -1;
        ResetReading();
    }
}

/************************************************************************/
/*                        GetFeatureCount()                             */
/************************************************************************/

GIntBig OGRFeatherLayer::GetFeatureCount(int bForce)
{
    if (m_poRecordBatchFileReader != nullptr && m_poAttrQuery == nullptr &&
        m_poFilterGeom == nullptr)
    {
        auto result = m_poRecordBatchFileReader->CountRows();
        if (result.ok())
            return *result;
    }
    else if (m_poRecordBatchReader != nullptr)
    {
        if (!m_bSeekable && !bForce)
        {
            if (m_poAttrQuery == nullptr && m_poFilterGeom == nullptr)
            {
                TryToCacheFirstTwoBatches();
            }

            if (!m_bSingleBatch)
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "GetFeatureCount() cannot be run in non-forced mode on "
                    "a non-seekable file made of several batches");
                return -1;
            }
        }

        if (m_poAttrQuery == nullptr && m_poFilterGeom == nullptr)
        {
            GIntBig nFeatures = 0;
            ResetReading();
            if (!m_poBatch)
                ReadNextBatchStream();
            while (m_poBatch)
            {
                nFeatures += m_poBatch->num_rows();
                if (!ReadNextBatchStream())
                    break;
            }
            ResetReading();
            return nFeatures;
        }
    }
    return OGRLayer::GetFeatureCount(bForce);
}

/************************************************************************/
/*                       CanRunNonForcedGetExtent()                     */
/************************************************************************/

bool OGRFeatherLayer::CanRunNonForcedGetExtent()
{
    if (m_bSeekable)
        return true;
    TryToCacheFirstTwoBatches();
    if (!m_bSingleBatch)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GetExtent() cannot be run in non-forced mode on "
                 "a non-seekable file made of several batches");
        return false;
    }
    return true;
}

/************************************************************************/
/*                         TestCapability()                             */
/************************************************************************/

int OGRFeatherLayer::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, OLCFastFeatureCount))
    {
        return m_bSeekable && m_poAttrQuery == nullptr &&
               m_poFilterGeom == nullptr;
    }

    if (EQUAL(pszCap, OLCMeasuredGeometries))
        return true;
    if (EQUAL(pszCap, OLCZGeometries))
        return true;

    return OGRArrowLayer::TestCapability(pszCap);
}

/************************************************************************/
/*                         GetMetadataItem()                            */
/************************************************************************/

const char *OGRFeatherLayer::GetMetadataItem(const char *pszName,
                                             const char *pszDomain)
{
    // Mostly for unit test purposes
    if (pszDomain != nullptr && EQUAL(pszDomain, "_ARROW_"))
    {
        if (EQUAL(pszName, "FORMAT"))
        {
            return m_poRecordBatchFileReader ? "FILE" : "STREAM";
        }
        if (m_poRecordBatchFileReader != nullptr)
        {
            int iBatch = -1;
            if (EQUAL(pszName, "NUM_RECORD_BATCHES"))
            {
                return CPLSPrintf(
                    "%d", m_poRecordBatchFileReader->num_record_batches());
            }
            else if (sscanf(pszName, "RECORD_BATCHES[%d]", &iBatch) == 1 &&
                     strstr(pszName, ".NUM_ROWS"))
            {
                auto result =
                    m_poRecordBatchFileReader->ReadRecordBatch(iBatch);
                if (!result.ok())
                {
                    return nullptr;
                }
                return CPLSPrintf("%" PRId64, (*result)->num_rows());
            }
        }
        return nullptr;
    }
    else if (pszDomain != nullptr && EQUAL(pszDomain, "_ARROW_METADATA_"))
    {
        const auto kv_metadata =
            (m_poRecordBatchFileReader ? m_poRecordBatchFileReader->schema()
                                       : m_poRecordBatchReader->schema())
                ->metadata();
        if (kv_metadata && kv_metadata->Contains(pszName))
        {
            auto metadataItem = kv_metadata->Get(pszName);
            if (metadataItem.ok())
            {
                return CPLSPrintf("%s", metadataItem->c_str());
            }
        }
        return nullptr;
    }
    else if (m_poRecordBatchFileReader != nullptr && pszDomain != nullptr &&
             EQUAL(pszDomain, "_ARROW_FOOTER_METADATA_"))
    {
        const auto kv_metadata = m_poRecordBatchFileReader->metadata();
        if (kv_metadata && kv_metadata->Contains(pszName))
        {
            auto metadataItem = kv_metadata->Get(pszName);
            if (metadataItem.ok())
            {
                return CPLSPrintf("%s", metadataItem->c_str());
            }
        }
        return nullptr;
    }
    return OGRLayer::GetMetadataItem(pszName, pszDomain);
}

/************************************************************************/
/*                           GetMetadata()                              */
/************************************************************************/

char **OGRFeatherLayer::GetMetadata(const char *pszDomain)
{
    // Mostly for unit test purposes
    if (pszDomain != nullptr && EQUAL(pszDomain, "_ARROW_METADATA_"))
    {
        m_aosFeatherMetadata.Clear();
        const auto kv_metadata =
            (m_poRecordBatchFileReader ? m_poRecordBatchFileReader->schema()
                                       : m_poRecordBatchReader->schema())
                ->metadata();
        if (kv_metadata)
        {
            for (const auto &kv : kv_metadata->sorted_pairs())
            {
                m_aosFeatherMetadata.SetNameValue(kv.first.c_str(),
                                                  kv.second.c_str());
            }
        }
        return m_aosFeatherMetadata.List();
    }
    if (m_poRecordBatchFileReader != nullptr && pszDomain != nullptr &&
        EQUAL(pszDomain, "_ARROW_FOOTER_METADATA_"))
    {
        m_aosFeatherMetadata.Clear();
        const auto kv_metadata = m_poRecordBatchFileReader->metadata();
        if (kv_metadata)
        {
            for (const auto &kv : kv_metadata->sorted_pairs())
            {
                m_aosFeatherMetadata.SetNameValue(kv.first.c_str(),
                                                  kv.second.c_str());
            }
        }
        return m_aosFeatherMetadata.List();
    }
    return OGRLayer::GetMetadata(pszDomain);
}
