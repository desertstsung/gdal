/******************************************************************************
 *
 * Project:  NDF Driver
 * Purpose:  Implementation of NLAPS Data Format read support.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2005, Frank Warmerdam
 * Copyright (c) 2008-2011, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_string.h"
#include "gdal_frmts.h"
#include "ogr_spatialref.h"
#include "rawdataset.h"

/************************************************************************/
/* ==================================================================== */
/*                              NDFDataset                              */
/* ==================================================================== */
/************************************************************************/

class NDFDataset final : public RawDataset
{
    GDALGeoTransform m_gt{};

    OGRSpatialReference m_oSRS{};
    char **papszExtraFiles;

    char **papszHeader;
    const char *Get(const char *pszKey, const char *pszDefault);

    CPL_DISALLOW_COPY_ASSIGN(NDFDataset)

    CPLErr Close() override;

  public:
    NDFDataset();
    ~NDFDataset() override;

    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;

    const OGRSpatialReference *GetSpatialRef() const override
    {
        return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
    }

    char **GetFileList(void) override;

    static GDALDataset *Open(GDALOpenInfo *);
    static int Identify(GDALOpenInfo *);
};

/************************************************************************/
/*                            NDFDataset()                             */
/************************************************************************/

NDFDataset::NDFDataset() : papszExtraFiles(nullptr), papszHeader(nullptr)
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

/************************************************************************/
/*                            ~NDFDataset()                             */
/************************************************************************/

NDFDataset::~NDFDataset()

{
    NDFDataset::Close();
}

/************************************************************************/
/*                              Close()                                 */
/************************************************************************/

CPLErr NDFDataset::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (NDFDataset::FlushCache(true) != CE_None)
            eErr = CE_Failure;

        CSLDestroy(papszHeader);
        CSLDestroy(papszExtraFiles);

        if (GDALPamDataset::Close() != CE_None)
            eErr = CE_Failure;
    }
    return eErr;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr NDFDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = m_gt;
    return CE_None;
}

/************************************************************************/
/*                                Get()                                 */
/*                                                                      */
/*      Fetch a value from the header by keyword.                       */
/************************************************************************/

const char *NDFDataset::Get(const char *pszKey, const char *pszDefault)

{
    const char *pszResult = CSLFetchNameValue(papszHeader, pszKey);

    if (pszResult == nullptr)
        return pszDefault;

    return pszResult;
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **NDFDataset::GetFileList()

{
    // Main data file, etc.
    char **papszFileList = GDALPamDataset::GetFileList();

    // Header file.
    papszFileList = CSLInsertStrings(papszFileList, -1, papszExtraFiles);

    return papszFileList;
}

/************************************************************************/
/*                            Identify()                                */
/************************************************************************/

int NDFDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    /* -------------------------------------------------------------------- */
    /*      The user must select the header file (i.e. .H1).                */
    /* -------------------------------------------------------------------- */
    return poOpenInfo->nHeaderBytes >= 50 &&
           (STARTS_WITH_CI(
                reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                "NDF_REVISION=2") ||
            STARTS_WITH_CI(
                reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                "NDF_REVISION=0"));
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *NDFDataset::Open(GDALOpenInfo *poOpenInfo)

{
    /* -------------------------------------------------------------------- */
    /*      The user must select the header file (i.e. .H1).                */
    /* -------------------------------------------------------------------- */
    if (!Identify(poOpenInfo) || poOpenInfo->fpL == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("NDF");
        return nullptr;
    }
    /* -------------------------------------------------------------------- */
    /*      Read and process the header into a local name/value             */
    /*      stringlist.  We just take off the trailing semicolon.  The      */
    /*      keyword is already separated from the value by an equal         */
    /*      sign.                                                           */
    /* -------------------------------------------------------------------- */

    const char *pszLine = nullptr;
    const int nHeaderMax = 1000;
    int nHeaderLines = 0;
    char **papszHeader =
        static_cast<char **>(CPLMalloc(sizeof(char *) * (nHeaderMax + 1)));

    while (nHeaderLines < nHeaderMax &&
           (pszLine = CPLReadLineL(poOpenInfo->fpL)) != nullptr &&
           !EQUAL(pszLine, "END_OF_HDR;"))
    {
        if (strstr(pszLine, "=") == nullptr)
            break;

        char *pszFixed = CPLStrdup(pszLine);
        if (pszFixed[strlen(pszFixed) - 1] == ';')
            pszFixed[strlen(pszFixed) - 1] = '\0';

        papszHeader[nHeaderLines++] = pszFixed;
        papszHeader[nHeaderLines] = nullptr;
    }
    CPL_IGNORE_RET_VAL(VSIFCloseL(poOpenInfo->fpL));
    poOpenInfo->fpL = nullptr;

    if (CSLFetchNameValue(papszHeader, "PIXELS_PER_LINE") == nullptr ||
        CSLFetchNameValue(papszHeader, "LINES_PER_DATA_FILE") == nullptr ||
        CSLFetchNameValue(papszHeader, "BITS_PER_PIXEL") == nullptr ||
        CSLFetchNameValue(papszHeader, "PIXEL_FORMAT") == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Dataset appears to be NDF but is missing a required field.");
        CSLDestroy(papszHeader);
        return nullptr;
    }

    if (!EQUAL(CSLFetchNameValue(papszHeader, "PIXEL_FORMAT"), "BYTE") ||
        !EQUAL(CSLFetchNameValue(papszHeader, "BITS_PER_PIXEL"), "8"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Currently NDF driver supports only 8bit BYTE format.");
        CSLDestroy(papszHeader);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        CSLDestroy(papszHeader);
        ReportUpdateNotSupportedByDriver("NDF");
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    auto poDS = std::make_unique<NDFDataset>();
    poDS->papszHeader = papszHeader;

    poDS->nRasterXSize = atoi(poDS->Get("PIXELS_PER_LINE", ""));
    poDS->nRasterYSize = atoi(poDS->Get("LINES_PER_DATA_FILE", ""));

    /* -------------------------------------------------------------------- */
    /*      Create a raw raster band for each file.                         */
    /* -------------------------------------------------------------------- */
    const char *pszBand =
        CSLFetchNameValue(papszHeader, "NUMBER_OF_BANDS_IN_VOLUME");
    if (pszBand == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot find band count");
        return nullptr;
    }
    const int nBands = atoi(pszBand);

    if (!GDALCheckDatasetDimensions(poDS->nRasterXSize, poDS->nRasterYSize) ||
        !GDALCheckBandCount(nBands, FALSE))
    {
        return nullptr;
    }

    for (int iBand = 0; iBand < nBands; iBand++)
    {
        char szKey[100];
        snprintf(szKey, sizeof(szKey), "BAND%d_FILENAME", iBand + 1);
        CPLString osFilename = poDS->Get(szKey, "");

        // NDF1 file do not include the band filenames.
        if (osFilename.empty())
        {
            char szBandExtension[15];
            snprintf(szBandExtension, sizeof(szBandExtension), "I%d",
                     iBand + 1);
            osFilename =
                CPLResetExtensionSafe(poOpenInfo->pszFilename, szBandExtension);
        }
        else
        {
            CPLString osBasePath = CPLGetPathSafe(poOpenInfo->pszFilename);
            osFilename = CPLFormFilenameSafe(osBasePath, osFilename, nullptr);
        }

        VSILFILE *fpRaw = VSIFOpenL(osFilename, "rb");
        if (fpRaw == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Failed to open band file: %s", osFilename.c_str());
            return nullptr;
        }
        poDS->papszExtraFiles = CSLAddString(poDS->papszExtraFiles, osFilename);

        auto poBand = RawRasterBand::Create(
            poDS.get(), iBand + 1, fpRaw, 0, 1, poDS->nRasterXSize, GDT_Byte,
            RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
            RawRasterBand::OwnFP::YES);
        if (!poBand)
            return nullptr;

        snprintf(szKey, sizeof(szKey), "BAND%d_NAME", iBand + 1);
        poBand->SetDescription(poDS->Get(szKey, ""));

        snprintf(szKey, sizeof(szKey), "BAND%d_WAVELENGTHS", iBand + 1);
        poBand->SetMetadataItem("WAVELENGTHS", poDS->Get(szKey, ""));

        snprintf(szKey, sizeof(szKey), "BAND%d_RADIOMETRIC_GAINS/BIAS",
                 iBand + 1);
        poBand->SetMetadataItem("RADIOMETRIC_GAINS_BIAS", poDS->Get(szKey, ""));

        poDS->SetBand(iBand + 1, std::move(poBand));
    }

    /* -------------------------------------------------------------------- */
    /*      Fetch and parse USGS projection parameters.                     */
    /* -------------------------------------------------------------------- */
    double adfUSGSParams[15] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const CPLStringList aosParamTokens(CSLTokenizeStringComplex(
        poDS->Get("USGS_PROJECTION_NUMBER", ""), ",", FALSE, TRUE));

    if (aosParamTokens.size() >= 15)
    {
        for (int i = 0; i < 15; i++)
            adfUSGSParams[i] = CPLAtof(aosParamTokens[i]);
    }

    /* -------------------------------------------------------------------- */
    /*      Minimal georef support ... should add full USGS style           */
    /*      support at some point.                                          */
    /* -------------------------------------------------------------------- */
    const int nUSGSProjection = atoi(poDS->Get("USGS_PROJECTION_NUMBER", ""));
    const int nZone = atoi(poDS->Get("USGS_MAP_ZONE", "0"));

    OGRSpatialReference oSRS;
    oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    oSRS.importFromUSGS(nUSGSProjection, nZone, adfUSGSParams, 12);

    const CPLString osDatum = poDS->Get("HORIZONTAL_DATUM", "");
    if (EQUAL(osDatum, "WGS84") || EQUAL(osDatum, "NAD83") ||
        EQUAL(osDatum, "NAD27"))
    {
        oSRS.SetWellKnownGeogCS(osDatum);
    }
    else if (STARTS_WITH_CI(osDatum, "NAD27"))
    {
        oSRS.SetWellKnownGeogCS("NAD27");
    }
    else
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Unrecognized datum name in NLAPS/NDF file:%s, "
                 "assuming WGS84.",
                 osDatum.c_str());
        oSRS.SetWellKnownGeogCS("WGS84");
    }

    if (oSRS.GetRoot() != nullptr)
    {
        poDS->m_oSRS = std::move(oSRS);
    }

    /* -------------------------------------------------------------------- */
    /*      Get geotransform.                                               */
    /* -------------------------------------------------------------------- */
    char **papszUL =
        CSLTokenizeString2(poDS->Get("UPPER_LEFT_CORNER", ""), ",", 0);
    char **papszUR =
        CSLTokenizeString2(poDS->Get("UPPER_RIGHT_CORNER", ""), ",", 0);
    char **papszLL =
        CSLTokenizeString2(poDS->Get("LOWER_LEFT_CORNER", ""), ",", 0);

    if (CSLCount(papszUL) == 4 && CSLCount(papszUR) == 4 &&
        CSLCount(papszLL) == 4)
    {
        poDS->m_gt[0] = CPLAtof(papszUL[2]);
        poDS->m_gt[1] = (CPLAtof(papszUR[2]) - CPLAtof(papszUL[2])) /
                        (poDS->nRasterXSize - 1);
        poDS->m_gt[2] = (CPLAtof(papszUR[3]) - CPLAtof(papszUL[3])) /
                        (poDS->nRasterXSize - 1);

        poDS->m_gt[3] = CPLAtof(papszUL[3]);
        poDS->m_gt[4] = (CPLAtof(papszLL[2]) - CPLAtof(papszUL[2])) /
                        (poDS->nRasterYSize - 1);
        poDS->m_gt[5] = (CPLAtof(papszLL[3]) - CPLAtof(papszUL[3])) /
                        (poDS->nRasterYSize - 1);

        // Move origin up-left half a pixel.
        poDS->m_gt[0] -= poDS->m_gt[1] * 0.5;
        poDS->m_gt[0] -= poDS->m_gt[4] * 0.5;
        poDS->m_gt[3] -= poDS->m_gt[2] * 0.5;
        poDS->m_gt[3] -= poDS->m_gt[5] * 0.5;
    }

    CSLDestroy(papszUL);
    CSLDestroy(papszLL);
    CSLDestroy(papszUR);

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS.get(), poOpenInfo->pszFilename);

    return poDS.release();
}

/************************************************************************/
/*                          GDALRegister_NDF()                          */
/************************************************************************/

void GDALRegister_NDF()

{
    if (GDALGetDriverByName("NDF") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("NDF");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "NLAPS Data Format");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/ndf.html");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnIdentify = NDFDataset::Identify;
    poDriver->pfnOpen = NDFDataset::Open;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
