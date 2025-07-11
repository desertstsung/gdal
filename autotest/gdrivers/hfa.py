#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test some functions of HFA driver.  Most testing in ../gcore/hfa_*
# Author:   Frank Warmerdam <warmerdam@pobox.com>
#
###############################################################################
# Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2011, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import math
import os
import shutil
import struct

import gdaltest
import pytest

from osgeo import gdal

pytestmark = pytest.mark.require_driver("HFA")

###############################################################################
# Verify we can read the special histogram metadata from a provided image.


def test_hfa_histread():

    ds = gdal.Open("../gcore/data/utmsmall.img")
    md = ds.GetRasterBand(1).GetMetadata()
    ds = None

    assert md["STATISTICS_MINIMUM"] == "8", "STATISTICS_MINIMUM is wrong."

    assert md["STATISTICS_MEDIAN"] == "148", "STATISTICS_MEDIAN is wrong."

    assert md["STATISTICS_HISTOMAX"] == "255", "STATISTICS_HISTOMAX is wrong."

    assert (
        md["STATISTICS_HISTOBINVALUES"]
        == "0|0|0|0|0|0|0|0|8|0|0|0|0|0|0|0|23|0|0|0|0|0|0|0|0|29|0|0|0|0|0|0|0|46|0|0|0|0|0|0|0|69|0|0|0|0|0|0|0|99|0|0|0|0|0|0|0|0|120|0|0|0|0|0|0|0|178|0|0|0|0|0|0|0|193|0|0|0|0|0|0|0|212|0|0|0|0|0|0|0|281|0|0|0|0|0|0|0|0|365|0|0|0|0|0|0|0|460|0|0|0|0|0|0|0|533|0|0|0|0|0|0|0|544|0|0|0|0|0|0|0|0|626|0|0|0|0|0|0|0|653|0|0|0|0|0|0|0|673|0|0|0|0|0|0|0|629|0|0|0|0|0|0|0|0|586|0|0|0|0|0|0|0|541|0|0|0|0|0|0|0|435|0|0|0|0|0|0|0|348|0|0|0|0|0|0|0|341|0|0|0|0|0|0|0|0|284|0|0|0|0|0|0|0|225|0|0|0|0|0|0|0|237|0|0|0|0|0|0|0|172|0|0|0|0|0|0|0|0|159|0|0|0|0|0|0|0|105|0|0|0|0|0|0|0|824|"
    ), "STATISTICS_HISTOBINVALUES is wrong."

    assert md["STATISTICS_SKIPFACTORX"] == "1", "STATISTICS_SKIPFACTORX is wrong."

    assert md["STATISTICS_SKIPFACTORY"] == "1", "STATISTICS_SKIPFACTORY is wrong."

    assert md["STATISTICS_EXCLUDEDVALUES"] == "0", "STATISTICS_EXCLUDEDVALUE is wrong."


###############################################################################
# Verify that if we copy this test image to a new Imagine file the histogram
# info is preserved.


def test_hfa_histwrite():

    drv = gdal.GetDriverByName("HFA")
    ds_src = gdal.Open("../gcore/data/utmsmall.img")
    out_ds = drv.CreateCopy("tmp/work.img", ds_src)
    del out_ds
    ds_src = None

    # Remove .aux.xml file as histogram can be written in it
    tmpAuxXml = "tmp/work.img.aux.xml"
    if os.path.exists(tmpAuxXml):
        os.remove(tmpAuxXml)

    ds = gdal.Open("tmp/work.img")
    md = ds.GetRasterBand(1).GetMetadata()
    ds = None

    drv.Delete("tmp/work.img")

    assert md["STATISTICS_MINIMUM"] == "8", "STATISTICS_MINIMUM is wrong."

    assert md["STATISTICS_MEDIAN"] == "148", "STATISTICS_MEDIAN is wrong."

    assert md["STATISTICS_HISTOMAX"] == "255", "STATISTICS_HISTOMAX is wrong."

    assert (
        md["STATISTICS_HISTOBINVALUES"]
        == "0|0|0|0|0|0|0|0|8|0|0|0|0|0|0|0|23|0|0|0|0|0|0|0|0|29|0|0|0|0|0|0|0|46|0|0|0|0|0|0|0|69|0|0|0|0|0|0|0|99|0|0|0|0|0|0|0|0|120|0|0|0|0|0|0|0|178|0|0|0|0|0|0|0|193|0|0|0|0|0|0|0|212|0|0|0|0|0|0|0|281|0|0|0|0|0|0|0|0|365|0|0|0|0|0|0|0|460|0|0|0|0|0|0|0|533|0|0|0|0|0|0|0|544|0|0|0|0|0|0|0|0|626|0|0|0|0|0|0|0|653|0|0|0|0|0|0|0|673|0|0|0|0|0|0|0|629|0|0|0|0|0|0|0|0|586|0|0|0|0|0|0|0|541|0|0|0|0|0|0|0|435|0|0|0|0|0|0|0|348|0|0|0|0|0|0|0|341|0|0|0|0|0|0|0|0|284|0|0|0|0|0|0|0|225|0|0|0|0|0|0|0|237|0|0|0|0|0|0|0|172|0|0|0|0|0|0|0|0|159|0|0|0|0|0|0|0|105|0|0|0|0|0|0|0|824|"
    ), "STATISTICS_HISTOBINVALUES is wrong."


###############################################################################
# Verify that if we copy this test image to a new Imagine file and then re-write the
# histogram information, the new histogram can then be read back in.


def test_hfa_histrewrite():

    drv = gdal.GetDriverByName("HFA")
    ds_src = gdal.Open("../gcore/data/utmsmall.img")
    out_ds = drv.CreateCopy("tmp/work.img", ds_src)
    del out_ds
    ds_src = None

    # Remove .aux.xml file as histogram can be written in it
    tmpAuxXml = "tmp/work.img.aux.xml"
    if os.path.exists(tmpAuxXml):
        os.remove(tmpAuxXml)

    # A new histogram which is different to what is in the file. It won't match the data,
    # but we are just testing the re-writing of the histogram, so we don't mind.
    newHist = "8|23|29|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|46|0|0|0|0|0|0|0|69|0|0|0|0|0|0|0|99|0|0|0|0|0|0|0|0|120|0|0|0|0|0|0|0|178|0|0|0|0|0|0|0|193|0|0|0|0|0|0|0|212|0|0|0|0|0|0|0|281|0|0|0|0|0|0|0|0|365|0|0|0|0|0|0|0|460|0|0|0|0|0|0|0|533|0|0|0|0|0|0|0|544|0|0|0|0|0|0|0|0|626|0|0|0|0|0|0|0|653|0|0|0|0|0|0|0|673|0|0|0|0|0|0|0|629|0|0|0|0|0|0|0|0|586|0|0|0|0|0|0|0|541|0|0|0|0|0|0|0|435|0|0|0|0|0|0|0|348|0|0|0|0|0|0|0|341|0|0|0|0|0|0|0|0|284|0|0|0|0|0|0|0|225|0|0|0|0|0|0|0|237|0|0|0|0|0|0|0|172|0|0|0|0|0|0|0|0|159|0|0|0|0|0|0|0|105|0|0|0|0|0|0|0|824|"

    ds = gdal.Open("tmp/work.img", gdal.GA_Update)
    band = ds.GetRasterBand(1)
    band.SetMetadataItem("STATISTICS_HISTOBINVALUES", newHist)
    ds = None

    if os.path.exists(tmpAuxXml):
        os.remove(tmpAuxXml)

    ds = gdal.Open("tmp/work.img")
    histStr = ds.GetRasterBand(1).GetMetadataItem("STATISTICS_HISTOBINVALUES")
    ds = None

    drv.Delete("tmp/work.img")

    assert histStr == newHist, "Rewritten STATISTICS_HISTOBINVALUES is wrong."


###############################################################################
# Verify we can read metadata of int.img.


def test_hfa_int_stats_1():

    ds = gdal.Open("data/hfa/int.img")
    md = ds.GetRasterBand(1).GetMetadata()
    ds = None

    assert md["STATISTICS_MINIMUM"] == "40918", "STATISTICS_MINIMUM is wrong."

    assert md["STATISTICS_MAXIMUM"] == "41134", "STATISTICS_MAXIMUM is wrong."

    assert md["STATISTICS_MEDIAN"] == "41017", "STATISTICS_MEDIAN is wrong."

    assert md["STATISTICS_MODE"] == "41013", "STATISTICS_MODE is wrong."

    assert md["STATISTICS_HISTOMIN"] == "40918", "STATISTICS_HISTOMIN is wrong."

    assert md["STATISTICS_HISTOMAX"] == "41134", "STATISTICS_HISTOMAX is wrong."

    assert md["LAYER_TYPE"] == "athematic", "LAYER_TYPE is wrong."


###############################################################################
# Verify we can read band statistics of int.img.


def test_hfa_int_stats_2():

    ds = gdal.Open("data/hfa/int.img")
    stats = ds.GetRasterBand(1).GetStatistics(False, True)
    ds = None

    tolerance = 0.0001

    assert stats[0] == pytest.approx(40918.0, abs=tolerance), "Minimum value is wrong."

    assert stats[1] == pytest.approx(41134.0, abs=tolerance), "Maximum value is wrong."

    assert stats[2] == pytest.approx(
        41019.784218148, abs=tolerance
    ), "Mean value is wrong."

    assert stats[3] == pytest.approx(
        44.637237445468, abs=tolerance
    ), "StdDev value is wrong."


###############################################################################
# Verify we can read metadata of float.img.


def test_hfa_float_stats_1():

    ds = gdal.Open("data/hfa/float.img")
    md = ds.GetRasterBand(1).GetMetadata()
    ds = None

    tolerance = 0.0001

    mini = float(md["STATISTICS_MINIMUM"])
    assert mini == pytest.approx(
        40.91858291626, abs=tolerance
    ), "STATISTICS_MINIMUM is wrong."

    maxi = float(md["STATISTICS_MAXIMUM"])
    assert maxi == pytest.approx(
        41.134323120117, abs=tolerance
    ), "STATISTICS_MAXIMUM is wrong."

    median = float(md["STATISTICS_MEDIAN"])
    assert median == pytest.approx(
        41.017182931304, abs=tolerance
    ), "STATISTICS_MEDIAN is wrong."

    mod = float(md["STATISTICS_MODE"])
    assert mod == pytest.approx(
        41.0104410499, abs=tolerance
    ), "STATISTICS_MODE is wrong."

    histMin = float(md["STATISTICS_HISTOMIN"])
    assert histMin == pytest.approx(
        40.91858291626, abs=tolerance
    ), "STATISTICS_HISTOMIN is wrong."

    histMax = float(md["STATISTICS_HISTOMAX"])
    assert histMax == pytest.approx(
        41.134323120117, abs=tolerance
    ), "STATISTICS_HISTOMAX is wrong."

    assert md["LAYER_TYPE"] == "athematic", "LAYER_TYPE is wrong."


###############################################################################
# Verify we can read band statistics of float.img.


def test_hfa_float_stats_2():

    ds = gdal.Open("data/hfa/float.img")
    stats = ds.GetRasterBand(1).GetStatistics(False, True)
    ds = None

    tolerance = 0.0001

    assert stats[0] == pytest.approx(
        40.91858291626, abs=tolerance
    ), "Minimum value is wrong."

    assert stats[1] == pytest.approx(
        41.134323120117, abs=tolerance
    ), "Maximum value is wrong."

    assert stats[2] == pytest.approx(
        41.020284249223, abs=tolerance
    ), "Mean value is wrong."

    assert stats[3] == pytest.approx(
        0.044636441749041, abs=tolerance
    ), "StdDev value is wrong."


###############################################################################
# Verify we can read image data.


def test_hfa_int_read():

    ds = gdal.Open("data/hfa/int.img")
    band = ds.GetRasterBand(1)
    cs = band.Checksum()
    band.ReadRaster(100, 100, 1, 1)
    ds = None

    assert cs == 6691, "Checksum value is wrong."


###############################################################################
# Verify we can read image data.


def test_hfa_float_read():

    ds = gdal.Open("data/hfa/float.img")
    band = ds.GetRasterBand(1)
    cs = band.Checksum()
    data = band.ReadRaster(100, 100, 1, 1)
    ds = None

    assert cs == 23529, "Checksum value is wrong."

    # Read raw data into tuple of float numbers
    import struct

    value = struct.unpack("f" * 1, data)[0]

    assert value == pytest.approx(
        41.021659851074219, abs=0.0001
    ), "Pixel value is wrong."


###############################################################################
# verify we can read PE_STRING coordinate system.


def test_hfa_pe_read():

    ds = gdal.Open("data/hfa/87test.img")
    wkt = ds.GetProjectionRef()
    expected = 'PROJCS["World_Cube",GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0],UNIT["Degree",0.0174532925199433]],PROJECTION["Cube"],PARAMETER["False_Easting",0],PARAMETER["False_Northing",0],PARAMETER["Central_Meridian",0],PARAMETER["Option",1],UNIT["metre",1,AUTHORITY["EPSG","9001"]],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'

    assert wkt == expected, "failed to read pe string as expected."


###############################################################################
# Verify we can write PE_STRING nodes.


def test_hfa_pe_write():

    drv = gdal.GetDriverByName("HFA")
    ds_src = gdal.Open("data/hfa/87test.img")
    out_ds = drv.CreateCopy("tmp/87test.img", ds_src)
    del out_ds
    ds_src = None

    expected = 'PROJCS["World_Cube",GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0],UNIT["Degree",0.0174532925199433]],PROJECTION["Cube"],PARAMETER["False_Easting",0],PARAMETER["False_Northing",0],PARAMETER["Central_Meridian",0],PARAMETER["Option",1],UNIT["metre",1,AUTHORITY["EPSG","9001"]],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'

    ds = gdal.Open("tmp/87test.img")
    wkt = ds.GetProjectionRef()

    if wkt != expected:
        print("")
        pytest.fail("failed to write pe string as expected.")

    ds = None
    drv.Delete("tmp/87test.img")


###############################################################################
# Verify we can write and read large metadata items.


def test_hfa_metadata_1(tmp_path):

    md1_img = str(tmp_path / "md_1.img")

    drv = gdal.GetDriverByName("HFA")
    ds = drv.Create(md1_img, 100, 150, 1, gdal.GDT_Byte)

    md_val = "0123456789" * 60
    md = {"test": md_val}
    ds.GetRasterBand(1).SetMetadata(md)
    ds = None

    ds = gdal.Open(md1_img)
    md = ds.GetRasterBand(1).GetMetadata()
    assert md["test"] == md_val, "got wrong metadata back"
    assert ds.FlushCache() == gdal.CE_None
    ds = None

    ###############################################################################
    # Verify that writing metadata multiple times does not result in duplicate
    # nodes.

    ds = gdal.Open(md1_img, gdal.GA_Update)
    md = ds.GetRasterBand(1).GetMetadata()
    md["test"] = "0123456789"
    md["xxx"] = "123"
    ds.GetRasterBand(1).SetMetadata(md)
    ds = None

    ds = gdal.Open(md1_img)
    md = ds.GetRasterBand(1).GetMetadata()
    assert "xxx" in md, "metadata rewrite seems not to have worked"

    assert md["xxx"] == "123" and md["test"] == "0123456789", "got wrong metadata back"

    ds = None


###############################################################################
# Verify we can grow the RRD list in cases where this requires
# moving the HFAEntry to the end of the file.  (bug #1109)


def test_hfa_grow_rrdlist():

    import shutil

    shutil.copyfile("data/hfa/bug_1109.img", "tmp/bug_1109.img")
    # os.system("copy data\\bug_1109.img tmp")

    # Add two overview levels.
    ds = gdal.Open("tmp/bug_1109.img", gdal.GA_Update)
    result = ds.BuildOverviews(overviewlist=[4, 8])
    ds = None

    assert result == 0, "BuildOverviews failed."

    # Verify overviews are now findable.
    ds = gdal.Open("tmp/bug_1109.img")
    assert ds.GetRasterBand(1).GetOverviewCount() == 3, "Overview count wrong."

    ds = None
    gdal.GetDriverByName("HFA").Delete("tmp/bug_1109.img")


###############################################################################
# Make sure an old .ige file is deleted when creating a new dataset. (#1784)


def test_hfa_clean_ige():

    # Create an imagine file, forcing creation of an .ige file.

    drv = gdal.GetDriverByName("HFA")
    src_ds = gdal.Open("data/byte.tif")

    out_ds = drv.CreateCopy("tmp/igetest.img", src_ds, options=["USE_SPILL=YES"])
    out_ds = None

    try:
        open("tmp/igetest.ige")
    except IOError:
        pytest.fail("ige file not created with USE_SPILL=YES")

    # confirm ige shows up in file list.
    ds = gdal.Open("tmp/igetest.img")
    filelist = ds.GetFileList()
    ds = None

    found = 0
    for item in filelist:
        if item[-11:] == "igetest.ige":
            found = 1

    if not found:
        print(filelist)
        pytest.fail("no igetest.ige in file list!")

    # Create a file without a spill file, and verify old ige cleaned up.

    out_ds = drv.CreateCopy("tmp/igetest.img", src_ds)
    del out_ds

    assert not os.path.exists("tmp/igetest.ige")

    drv.Delete("tmp/igetest.img")


###############################################################################
# Verify that we can read this corrupt .aux file without hanging (#1907)


def test_hfa_corrupt_aux():

    # NOTE: we depend on being able to open .aux files as a weak sort of
    # dataset.

    with gdaltest.disable_exceptions(), gdal.quiet_errors():
        ds = gdal.Open("data/hfa/F0116231.aux")

        assert ds.RasterXSize == 1104, "did not get expected dataset characteristics"

        assert (
            gdal.GetLastErrorType() == 2
            and gdal.GetLastErrorMsg().find("Corrupt (looping)") != -1
        ), "Did not get expected warning."

    ds = None


###############################################################################
# support MapInformation for units (#1967)


def test_hfa_mapinformation_units():

    # NOTE: we depend on being able to open .aux files as a weak sort of
    # dataset.

    with gdal.quiet_errors():
        ds = gdal.Open("data/hfa/fg118-91.aux")

    wkt = ds.GetProjectionRef()
    expected_wkt = """PROJCS["NAD_1983_StatePlane_Virginia_North_FIPS_4501_Feet",GEOGCS["GCS_North_American_1983",DATUM["North_American_Datum_1983",SPHEROID["GRS_1980",6378137,298.257222101]],PRIMEM["Greenwich",0],UNIT["Degree",0.0174532925199432955],AUTHORITY["EPSG","4269"]],PROJECTION["Lambert_Conformal_Conic_2SP"],PARAMETER["False_Easting",11482916.66666666],PARAMETER["False_Northing",6561666.666666666],PARAMETER["Central_Meridian",-78.5],PARAMETER["Standard_Parallel_1",38.03333333333333],PARAMETER["Standard_Parallel_2",39.2],PARAMETER["Latitude_Of_Origin",37.66666666666666],UNIT["Foot_US",0.304800609601219241]]"""

    if gdaltest.equal_srs_from_wkt(expected_wkt, wkt):
        return
    pytest.fail()


###############################################################################
# Write nodata value.


def test_hfa_nodata_write(tmp_path):

    nodata_img = str(tmp_path / "nodata.img")

    drv = gdal.GetDriverByName("HFA")
    ds = drv.Create(nodata_img, 7, 7, 1, gdal.GDT_Byte)

    p = [1, 2, 1, 4, 1, 2, 1]
    raw_data = b"".join(struct.pack("h", x) for x in p)

    for line in range(7):
        ds.WriteRaster(0, line, 7, 1, raw_data, buf_type=gdal.GDT_Int16)

    b = ds.GetRasterBand(1)
    b.SetNoDataValue(1)

    ds = None

    ###############################################################################
    # Verify written nodata value.

    ds = gdal.Open(nodata_img)
    b = ds.GetRasterBand(1)

    assert b.GetNoDataValue() == 1, "failed to preserve nodata value"

    stats = b.GetStatistics(False, True)

    tolerance = 0.0001

    assert stats[0] == pytest.approx(2, abs=tolerance), "Minimum value is wrong."

    assert stats[1] == pytest.approx(4, abs=tolerance), "Maximum value is wrong."

    assert stats[2] == pytest.approx(
        2.6666666666667, abs=tolerance
    ), "Mean value is wrong."

    assert stats[3] == pytest.approx(
        0.94280904158206, abs=tolerance
    ), "StdDev value is wrong."

    b = None
    ds = None


###############################################################################
# Verify we read simple affine geotransforms properly.


def test_hfa_rotated_read():

    ds = gdal.Open("data/hfa/fg118-91.aux")

    check_gt = (
        11856857.07898215,
        0.895867662235625,
        0.02684252936279331,
        7041861.472946444,
        0.01962103617166367,
        -0.9007880319529181,
    )

    gt_epsilon = (abs(check_gt[1]) + abs(check_gt[2])) / 100.0

    new_gt = ds.GetGeoTransform()
    for i in range(6):
        if new_gt[i] != pytest.approx(check_gt[i], abs=gt_epsilon):
            print("")
            print("old = ", check_gt)
            print("new = ", new_gt)
            pytest.fail("Geotransform differs.")

    ds = None


###############################################################################
# Verify we can write affine geotransforms.


def test_hfa_rotated_write():

    # make sure we aren't preserving info in .aux.xml file
    try:
        os.remove("tmp/rot.img.aux.xml")
    except OSError:
        pass

    drv = gdal.GetDriverByName("HFA")
    ds = drv.Create("tmp/rot.img", 100, 150, 1, gdal.GDT_Byte)

    check_gt = (
        11856857.07898215,
        0.895867662235625,
        0.02684252936279331,
        7041861.472946444,
        0.01962103617166367,
        -0.9007880319529181,
    )

    expected_wkt = """PROJCS["NAD83 / Virginia North",
    GEOGCS["NAD83",
        DATUM["North_American_Datum_1983",
            SPHEROID["GRS 1980",6378137,298.257222101,
                AUTHORITY["EPSG","7019"]],
            AUTHORITY["EPSG","6269"]],
        PRIMEM["Greenwich",0,
            AUTHORITY["EPSG","8901"]],
        UNIT["degree",0.01745329251994328,
            AUTHORITY["EPSG","9122"]],
        AUTHORITY["EPSG","4269"]],
    PROJECTION["Lambert_Conformal_Conic_2SP"],
    PARAMETER["standard_parallel_1",39.2],
    PARAMETER["standard_parallel_2",38.03333333333333],
    PARAMETER["latitude_of_origin",37.66666666666666],
    PARAMETER["central_meridian",-78.5],
    PARAMETER["false_easting",11482916.66666667],
    PARAMETER["false_northing",6561666.666666667],
    UNIT["us_survey_feet",0.3048006096012192]]"""

    # For some reason we are now no longer able to preserve the authority
    # nodes and other info in the above, so we revert to the following.
    # (see #2755 for followup).

    expected_wkt = """PROJCS["NAD83_Virginia_North",GEOGCS["GCS_North_American_1983",DATUM["North_American_Datum_1983",SPHEROID["GRS_1980",6378137,298.257222101]],PRIMEM["Greenwich",0],UNIT["Degree",0.017453292519943295]],PROJECTION["Lambert_Conformal_Conic_2SP"],PARAMETER["standard_parallel_1",39.2],PARAMETER["standard_parallel_2",38.03333333333333],PARAMETER["latitude_of_origin",37.66666666666666],PARAMETER["central_meridian",-78.5],PARAMETER["false_easting",11482916.66666667],PARAMETER["false_northing",6561666.666666667],PARAMETER["scale_factor",1.0],UNIT["Foot_US",0.30480060960121924]]"""

    ds.SetGeoTransform(check_gt)
    ds.SetProjection(expected_wkt)

    ds = None

    ds = gdal.Open("tmp/rot.img")
    gt_epsilon = (abs(check_gt[1]) + abs(check_gt[2])) / 100.0

    new_gt = ds.GetGeoTransform()
    for i in range(6):
        if new_gt[i] != pytest.approx(check_gt[i], abs=gt_epsilon):
            print("")
            print("old = ", check_gt)
            print("new = ", new_gt)
            pytest.fail("Geotransform differs.")

    wkt = ds.GetProjection()
    assert gdaltest.equal_srs_from_wkt(expected_wkt, wkt)

    ds = None

    gdal.GetDriverByName("HFA").Delete("tmp/rot.img")


###############################################################################
# Test creating an in memory copy.


def test_hfa_vsimem():

    tst = gdaltest.GDALTest("HFA", "byte.tif", 1, 4672)

    tst.testCreateCopy(vsimem=1)


###############################################################################
# Test that PROJCS[] names are preserved as the mapinfo.proName in
# the .img file.  (#2422)


@pytest.mark.skipif(
    not gdaltest.vrt_has_open_support(),
    reason="VRT driver open missing",
)
def test_hfa_proName():

    drv = gdal.GetDriverByName("HFA")
    with gdaltest.config_option("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL"):
        src_ds = gdal.Open("data/hfa/stateplane.vrt")
    dst_ds = drv.CreateCopy("tmp/proname.img", src_ds)

    del dst_ds
    src_ds = None

    # Make sure we don't have interference from an .aux.xml
    try:
        os.remove("tmp/proname.img.aux.xml")
    except OSError:
        pass

    ds = gdal.Open("tmp/proname.img")

    srs = ds.GetProjectionRef()
    assert srs.startswith('PROJCS["NAD83 / Ohio South (ftUS)",')

    ds = None

    drv.Delete("tmp/proname.img")


###############################################################################
# Read a compressed file where no block has been written (#2523)


def test_hfa_read_empty_compressed():

    drv = gdal.GetDriverByName("HFA")
    ds = drv.Create("tmp/emptycompressed.img", 64, 64, 1, options=["COMPRESSED=YES"])
    ds = None

    ds = gdal.Open("tmp/emptycompressed.img")
    assert ds.GetRasterBand(1).Checksum() == 0
    ds = None

    drv.Delete("tmp/emptycompressed.img")


###############################################################################
# Verify "unique values" based color table (#2419)


def test_hfa_unique_values_color_table():

    ds = gdal.Open("data/hfa/i8u_c_i.img")

    ct = ds.GetRasterBand(1).GetRasterColorTable()

    assert ct.GetCount() == 256, "got wrong color count"

    assert (
        ct.GetColorEntry(253) == (0, 0, 0, 0)
        and ct.GetColorEntry(254) == (255, 255, 170, 255)
        and ct.GetColorEntry(255) == (255, 255, 255, 255)
    ), "Got wrong colors"

    ct = None
    ds = None


###############################################################################
# Verify "unique values" based histogram.


def test_hfa_unique_values_hist():

    try:
        gdal.RasterAttributeTable()
    except Exception:
        pytest.skip()

    ds = gdal.Open("data/hfa/i8u_c_i.img")

    md = ds.GetRasterBand(1).GetMetadata()

    expected = "12603|1|0|0|45|1|0|0|0|0|656|177|0|0|5026|1062|0|0|2|0|0|0|0|0|0|0|0|0|0|0|0|0|75|1|0|0|207|158|0|0|8|34|0|0|0|0|538|57|0|10|214|20|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|1|31|0|0|9|625|67|0|0|118|738|117|3004|1499|491|187|1272|513|1|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|16|3|0|0|283|123|5|1931|835|357|332|944|451|80|40|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|12|5|0|0|535|1029|118|0|33|246|342|0|0|10|8|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|169|439|0|0|6|990|329|0|0|120|295|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|164|42|0|0|570|966|0|0|18|152|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|45|106|0|0|16|16517|"
    assert md["STATISTICS_HISTOBINVALUES"] == expected, "Unexpected HISTOBINVALUES."

    assert (
        md["STATISTICS_HISTOMIN"] == "0" and md["STATISTICS_HISTOMAX"] == "255"
    ), "unexpected histomin/histomax value."

    # lets also check the RAT to ensure it has the BinValues column added.

    rat = ds.GetRasterBand(1).GetDefaultRAT()

    assert (
        rat.GetColumnCount() == 6
        and rat.GetTypeOfCol(0) == gdal.GFT_Real
        and rat.GetUsageOfCol(0) == gdal.GFU_MinMax
    ), "BinValues column wrong."

    assert rat.GetValueAsInt(2, 0) == 4, "BinValues value wrong."

    rat = None

    ds = None


###############################################################################
# Verify reading of 3rd order XFORM polynomials.


def test_hfa_xforms_3rd():

    ds = gdal.Open("data/hfa/42BW_420730_VT2.aux")

    check_list = [
        ("XFORM_STEPS", 2),
        ("XFORM0_POLYCOEFMTX[3]", -0.151286406053458),
        ("XFORM0_POLYCOEFVECTOR[1]", 401692.559078924),
        ("XFORM1_ORDER", 3),
        ("XFORM1_FWD_POLYCOEFMTX[0]", -0.560405515080768),
        ("XFORM1_FWD_POLYCOEFMTX[17]", -1.01593898110617e-08),
        ("XFORM1_REV_POLYCOEFMTX[17]", 4.01319402177037e-09),
        ("XFORM1_REV_POLYCOEFVECTOR[0]", 2605.41812438735),
    ]

    xform_md = ds.GetMetadata("XFORMS")

    for check_item in check_list:
        try:
            value = float(xform_md[check_item[0]])
        except (TypeError, ValueError):
            pytest.fail("metadata item %d missing" % check_item[0])

        assert value == pytest.approx(
            check_item[1], abs=abs(value / 100000.0)
        ), "metadata item %s has wrong value: %.15g" % (check_item[0], value)

    # Check that the GCPs are as expected implying that the evaluation
    # function for XFORMs if working ok.

    gcps = ds.GetGCPs()

    assert (
        gcps[0].GCPPixel == 0.5
        and gcps[0].GCPLine == 0.5
        and gcps[0].GCPX == pytest.approx(1667635.007, abs=0.001)
        and gcps[0].GCPY == pytest.approx(2620003.171, abs=0.001)
    ), "GCP 0 value wrong."

    assert (
        gcps[14].GCPPixel == pytest.approx(1769.7, abs=0.1)
        and gcps[14].GCPLine == pytest.approx(2124.9, abs=0.1)
        and gcps[14].GCPX == pytest.approx(1665221.064, abs=0.001)
        and gcps[14].GCPY == pytest.approx(2632414.379, abs=0.001)
    ), "GCP 14 value wrong."

    ds = None


###############################################################################
# Verify that we can clear an existing color table


def test_hfa_delete_colortable():
    # copy a file to tmp dir to modify.
    open("tmp/i8u.img", "wb").write(open("data/hfa/i8u_c_i.img", "rb").read())

    # clear color table.
    ds = gdal.Open("tmp/i8u.img", gdal.GA_Update)

    try:
        ds.GetRasterBand(1).SetColorTable
    except Exception:
        # OG python bindings don't have SetColorTable, and if we use
        # SetRasterColorTable, it doesn't work either as None isn't a valid
        # value for them
        ds = None
        gdal.GetDriverByName("HFA").Delete("tmp/i8u.img")
        pytest.skip()

    ds.GetRasterBand(1).SetColorTable(None)
    ds = None

    # check color table gone.
    ds = gdal.Open("tmp/i8u.img")
    assert ds.GetRasterBand(1).GetColorTable() is None, "failed to remove color table"

    ds = None

    gdal.GetDriverByName("HFA").Delete("tmp/i8u.img")


###############################################################################
# Verify that we can clear an existing color table (#2842)


@pytest.mark.require_driver("BMP")
def test_hfa_delete_colortable2():

    # copy a file to tmp dir to modify.
    src_ds = gdal.Open("../gcore/data/8bit_pal.bmp")
    ds = gdal.GetDriverByName("HFA").CreateCopy(
        "tmp/hfa_delete_colortable2.img", src_ds
    )
    src_ds = None
    ds = None

    # clear color table.
    ds = gdal.Open("tmp/hfa_delete_colortable2.img", gdal.GA_Update)

    try:
        ds.GetRasterBand(1).SetColorTable
    except Exception:
        # OG python bindings don't have SetColorTable, and if we use
        # SetRasterColorTable, it doesn't work either as None isn't a valid
        # value for them
        ds = None
        gdal.GetDriverByName("HFA").Delete("tmp/hfa_delete_colortable2.img")
        pytest.skip()

    ds.GetRasterBand(1).SetColorTable(None)
    ds = None

    # check color table gone.
    ds = gdal.Open("tmp/hfa_delete_colortable2.img")
    assert ds.GetRasterBand(1).GetColorTable() is None, "failed to remove color table"

    ds = None

    gdal.GetDriverByName("HFA").Delete("tmp/hfa_delete_colortable2.img")


###############################################################################
# Verify we can read the special histogram metadata from a provided image.


def test_hfa_excluded_values():

    ds = gdal.Open("data/hfa/dem10.img")
    md = ds.GetRasterBand(1).GetMetadata()
    ds = None

    assert (
        md["STATISTICS_EXCLUDEDVALUES"] == "0,8,9"
    ), "STATISTICS_EXCLUDEDVALUE is wrong."


###############################################################################
# verify that we propagate nodata to overviews in .hfa/.rrd format.


@pytest.mark.require_driver("AAIGRID")
def test_hfa_ov_nodata():

    drv = gdal.GetDriverByName("HFA")
    src_ds = gdal.Open("data/aaigrid/nodata_int.asc")
    wrk_ds = drv.CreateCopy("/vsimem/ov_nodata.img", src_ds)
    src_ds = None

    wrk_ds.BuildOverviews(overviewlist=[2])
    wrk_ds = None

    wrk2_ds = gdal.Open("/vsimem/ov_nodata.img")
    ovb = wrk2_ds.GetRasterBand(1).GetOverview(0)

    assert ovb.GetNoDataValue() == -99999, "nodata not propagated to .img overview."

    assert ovb.GetMaskFlags() == gdal.GMF_NODATA, "mask flag not as expected."

    # Confirm that a .ovr file was *not* produced.
    with gdal.quiet_errors():
        try:
            wrk3_ds = gdal.Open("/vsimem/ov_nodata.img.ovr")
        except Exception:
            wrk3_ds = None

    assert (
        wrk3_ds is None
    ), "this test result is invalid since .ovr file was created, why?"

    wrk2_ds = None
    drv.Delete("/vsimem/ov_nodata.img")


###############################################################################
# Confirm that we can read 8bit grayscale overviews for 1bit images.


def test_hfa_read_bit2grayscale():

    ds = gdal.Open("data/hfa/small1bit.img")
    band = ds.GetRasterBand(1)
    ov = band.GetOverview(0)

    assert ov.Checksum() == 4247, "did not get expected overview checksum"

    ds_md = ds.GetMetadata()
    assert (
        ds_md["PyramidResamplingType"] == "AVERAGE_BIT2GRAYSCALE"
    ), "wrong pyramid resampling type metadata."


###############################################################################
# Confirm that we can create overviews in rrd format for an .img file with
# the bit2grayscale algorithm (#2914)


def test_hfa_write_bit2grayscale():

    import shutil

    shutil.copyfile("data/hfa/small1bit.img", "tmp/small1bit.img")
    shutil.copyfile("data/hfa/small1bit.rrd", "tmp/small1bit.rrd")

    with gdal.config_options({"USE_RRD": "YES", "HFA_USE_RRD": "YES"}):

        ds = gdal.Open("tmp/small1bit.img", gdal.GA_Update)
        ds.BuildOverviews(resampling="average_bit2grayscale", overviewlist=[2])

        ov = ds.GetRasterBand(1).GetOverview(1)

        assert ov.Checksum() == 57325, "wrong checksum for greyscale overview."

        ds = None

        gdal.GetDriverByName("HFA").Delete("tmp/small1bit.img")

    # as an aside, confirm the .rrd file was deleted.
    assert not os.path.exists("tmp/small1bit.rrd")


###############################################################################
# Verify handling of camera model metadata (#2675)


def test_hfa_camera_md():

    ds = gdal.Open("/vsisparse/data/hfa/251_sparse.xml")

    md = ds.GetMetadata("CAMERA_MODEL")

    check_list = [
        ("direction", "EMOD_FORWARD"),
        ("forSrcAffine[0]", "0.025004093931786"),
        ("invDstAffine[0]", "1"),
        ("coeffs[1]", "-0.008"),
        ("elevationType", "EPRJ_ELEVATION_TYPE_HEIGHT"),
    ]
    for check_item in check_list:
        try:
            value = md[check_item[0]]
        except IndexError:
            pytest.fail("metadata item %d missing" % check_item[0])

        assert value == check_item[1], "metadata item %s has wrong value: %s" % (
            check_item[0],
            value,
        )

    # Check that the SRS is reasonable.

    srs_wkt = md["outputProjection"]
    exp_wkt = 'PROJCS["UTM Zone 17, Northern Hemisphere",GEOGCS["NAD27",DATUM["North_American_Datum_1927",SPHEROID["Clarke 1866",6378206.4,294.978698213898,AUTHORITY["EPSG","7008"]],TOWGS84[-10,158,187,0,0,0,0],AUTHORITY["EPSG","6267"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9108"]],AUTHORITY["EPSG","4267"]],PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",-81],PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],PARAMETER["false_northing",0],UNIT["Meter",1],AUTHORITY["EPSG","26717"]]'

    assert gdaltest.equal_srs_from_wkt(srs_wkt, exp_wkt), "wrong outputProjection"

    ds = None


###############################################################################
# Verify dataset's projection matches expected


def hfa_verify_dataset_projection(dataset_path, exp_wkt):

    ds = gdal.Open(dataset_path)
    srs_wkt = ds.GetProjectionRef()
    assert gdaltest.equal_srs_from_wkt(exp_wkt, srs_wkt), "wrong outputProjection"

    ds = None


###############################################################################
# Verify can read Transverse Mercator (South Orientated) projections


def test_hfa_read_tmso_projection():
    exp_wkt = 'PROJCS["Transverse Mercator (South Orientated)",GEOGCS["Cape-1",DATUM["Cape-1",SPHEROID["Clarke 1880 Arc",6378249.145,293.4663077168331],TOWGS84[-136,-108,-292,0,0,0,0]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],PROJECTION["Transverse_Mercator_South_Orientated"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",21],PARAMETER["scale_factor",1],PARAMETER["false_easting",0],PARAMETER["false_northing",0],UNIT["meters",1]]'
    return hfa_verify_dataset_projection("../gcore/data/22281.aux", exp_wkt)


###############################################################################
# Verify can write Transverse Mercator (South Orientated) projections to aux files


def test_hfa_write_tmso_projection():
    dataset_path = "tmp/tmso.img"
    out_ds = gdal.GetDriverByName("HFA").Create(dataset_path, 1, 1)
    gt = (0, 1, 0, 0, 0, 1)
    out_ds.SetGeoTransform(gt)
    out_ds.SetProjection(
        'PROJCS["Hartebeesthoek94 / Lo15",GEOGCS["Hartebeesthoek94",DATUM["Hartebeesthoek94",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],TOWGS84[0,0,0,0,0,0,0],AUTHORITY["EPSG","6148"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.01745329251994328,AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4148"]],UNIT["metre",1,AUTHORITY["EPSG","9001"]],PROJECTION["Transverse_Mercator_South_Orientated"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",15],PARAMETER["scale_factor",1],PARAMETER["false_easting",0],PARAMETER["false_northing",0],AUTHORITY["EPSG","2046"],AXIS["Y",WEST],AXIS["X",SOUTH]]'
    )
    out_ds = None
    exp_wkt = 'PROJCS["Hartebeesthoek94 / Lo15",GEOGCS["Hartebeesthoek94",DATUM["Hartebeesthoek94",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],TOWGS84[0,0,0,0,0,0,0],AUTHORITY["EPSG","6148"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.01745329251994328,AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4148"]],UNIT["metre",1,AUTHORITY["EPSG","9001"]],PROJECTION["Transverse_Mercator_South_Orientated"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",15],PARAMETER["scale_factor",1],PARAMETER["false_easting",0],PARAMETER["false_northing",0],AUTHORITY["EPSG","2046"],AXIS["Y",WEST],AXIS["X",SOUTH]]'
    hfa_verify_dataset_projection(dataset_path, exp_wkt)
    gdal.GetDriverByName("HFA").Delete(dataset_path)


###############################################################################
# Verify can read Hotine Oblique Mercator (Variant A) projections


def test_hfa_read_homva_projection():
    exp_wkt = 'PROJCS["Hotine Oblique Mercator (Variant A)",GEOGCS["GDM 2000",DATUM["Geodetic_Datum_of_Malaysia_2000",SPHEROID["GRS 1980",6378137,298.257222096042],TOWGS84[0,0,0,0,0,0,0]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]]],PROJECTION["Hotine_Oblique_Mercator"],PARAMETER["latitude_of_center",4],PARAMETER["longitude_of_center",115],PARAMETER["azimuth",53.31580995],PARAMETER["rectified_grid_angle",53.1301023611111],PARAMETER["scale_factor",0.99984],PARAMETER["false_easting",0],PARAMETER["false_northing",0],UNIT["meters",1],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
    ds = gdal.Open("../gcore/data/3376.tif")
    srs_wkt = ds.GetProjectionRef()
    assert gdaltest.equal_srs_from_wkt(
        srs_wkt, exp_wkt, verbose=False
    ) or gdaltest.equal_srs_from_wkt(
        srs_wkt,
        exp_wkt.replace("Geodetic_Datum_of_Malaysia_2000", "GDM 2000"),
        verbose=False,
    ), srs_wkt


###############################################################################
# Verify can write  Hotine Oblique Mercator (Variant A) projections to aux files


def test_hfa_write_homva_projection():
    dataset_path = "tmp/homva.img"
    out_ds = gdal.GetDriverByName("HFA").Create(dataset_path, 1, 1)
    gt = (0, 1, 0, 0, 0, 1)
    out_ds.SetGeoTransform(gt)
    out_ds.SetProjection(
        'PROJCS["Hotine Oblique Mercator (Variant A)",GEOGCS["GDM 2000",DATUM["GDM_2000",SPHEROID["GRS 1980",6378137,298.2572220960422],TOWGS84[0,0,0,0,0,0,0]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],PROJECTION["Hotine_Oblique_Mercator"],PARAMETER["latitude_of_center",4],PARAMETER["longitude_of_center",115],PARAMETER["azimuth",53.31580995],PARAMETER["rectified_grid_angle",53.13010236111111],PARAMETER["scale_factor",0.99984],PARAMETER["false_easting",0],PARAMETER["false_northing",0],UNIT["meters",1]]'
    )
    out_ds = None
    exp_wkt = 'PROJCS["Hotine Oblique Mercator (Variant A)",GEOGCS["GDM_2000",DATUM["GDM_2000",SPHEROID["GRS 1980",6378137,298.257222096042],TOWGS84[0,0,0,0,0,0,0]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]]],PROJECTION["Hotine_Oblique_Mercator"],PARAMETER["latitude_of_center",4],PARAMETER["longitude_of_center",115],PARAMETER["azimuth",53.31580995],PARAMETER["rectified_grid_angle",53.1301023611111],PARAMETER["scale_factor",0.99984],PARAMETER["false_easting",0],PARAMETER["false_northing",0],UNIT["meters",1],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
    hfa_verify_dataset_projection(dataset_path, exp_wkt)
    gdal.GetDriverByName("HFA").Delete(dataset_path)


###############################################################################
# Check that overviews with an .rde file are properly supported in file list,
# and fetching actual overviews.


def test_hfa_rde_overviews():

    # Create an imagine file, forcing creation of an .ige file.

    ds = gdal.Open("data/hfa/spill.img")

    exp_cs = 1631
    cs = ds.GetRasterBand(1).Checksum()

    assert exp_cs == cs, "did not get expected band checksum"

    exp_cs = 340
    cs = ds.GetRasterBand(1).GetOverview(0).Checksum()

    assert exp_cs == cs, "did not get expected overview checksum"

    filelist = ds.GetFileList()
    exp_filelist = [
        "data/hfa/spill.img",
        "data/hfa/spill.ige",
        "data/hfa/spill.rrd",
        "data/hfa/spill.rde",
    ]
    assert [x.replace("\\", "/") for x in filelist] == exp_filelist

    ds = None


###############################################################################
# Check that we can copy and rename a complex file set, and that the internal filenames
# in the .img and .rrd seem to be updated properly.


def test_hfa_copyfiles():

    drv = gdal.GetDriverByName("HFA")
    drv.CopyFiles("tmp/newnamexxx_after_copy.img", "data/hfa/spill.img")

    drv.Rename("tmp/newnamexxx.img", "tmp/newnamexxx_after_copy.img")

    ds = gdal.Open("tmp/newnamexxx.img")

    exp_cs = 340
    cs = ds.GetRasterBand(1).GetOverview(0).Checksum()

    assert exp_cs == cs, "did not get expected overview checksum"

    filelist = ds.GetFileList()
    exp_filelist = [
        "tmp/newnamexxx.img",
        "tmp/newnamexxx.ige",
        "tmp/newnamexxx.rrd",
        "tmp/newnamexxx.rde",
    ]
    exp_filelist_win32 = [
        "tmp/newnamexxx.img",
        "tmp\\newnamexxx.ige",
        "tmp\\newnamexxx.rrd",
        "tmp\\newnamexxx.rde",
    ]
    assert (
        filelist == exp_filelist or filelist == exp_filelist_win32
    ), "did not get expected file list."

    ds = None

    # Check that the filenames in the actual files seem to have been updated.
    img = open("tmp/newnamexxx.img", "rb").read()
    img = str(img)
    assert img.find("newnamexxx.rrd") != -1, "RRDNames not updated?"

    assert img.find("newnamexxx.ige") != -1, "spill file not updated?"

    rrd = open("tmp/newnamexxx.rrd", "rb").read()
    rrd = str(rrd)
    assert rrd.find("newnamexxx.img") != -1, "DependentFile not updated?"

    assert rrd.find("newnamexxx.rde") != -1, "overview spill file not updated?"

    drv.Delete("tmp/newnamexxx.img")


###############################################################################
# Test the ability to write a RAT (#999)


def test_hfa_write_rat():

    drv = gdal.GetDriverByName("HFA")

    src_ds = gdal.Open("data/hfa/i8u_c_i.img")

    rat = src_ds.GetRasterBand(1).GetDefaultRAT()

    dst_ds = drv.Create("tmp/write_rat.img", 100, 100, 1, gdal.GDT_Byte)

    dst_ds.GetRasterBand(1).SetDefaultRAT(rat)

    dst_ds = None
    src_ds = None

    rat = None

    ds = gdal.Open("tmp/write_rat.img")
    rat = ds.GetRasterBand(1).GetDefaultRAT()

    assert (
        rat.GetColumnCount() == 6
        and rat.GetTypeOfCol(0) == gdal.GFT_Real
        and rat.GetUsageOfCol(0) == gdal.GFU_Generic
    ), "BinValues column wrong."

    assert rat.GetValueAsInt(2, 0) == 4, "BinValues value wrong."

    assert rat.GetValueAsInt(4, 5) == 656, "Histogram value wrong."

    rat = None
    ds = None

    drv.Delete("tmp/write_rat.img")


###############################################################################
# Test STATISTICS creation option


def test_hfa_createcopy_statistics(tmp_path):

    tmp_tif = str(tmp_path / "byte.tif")
    shutil.copy("../gcore/data/byte.tif", tmp_tif)

    ds_src = gdal.Open(tmp_tif)
    out_ds = gdal.GetDriverByName("HFA").CreateCopy(
        "/vsimem/byte.img", ds_src, options=["STATISTICS=YES"]
    )
    del out_ds
    ds_src = None

    gdal.Unlink("/vsimem/byte.img.aux.xml")

    ds = gdal.Open("/vsimem/byte.img")
    md = ds.GetRasterBand(1).GetMetadata()
    ds = None

    gdal.GetDriverByName("HFA").Delete("/vsimem/byte.img")

    assert md["STATISTICS_MINIMUM"] == "74", "STATISTICS_MINIMUM is wrong."


###############################################################################
# Test GetUnitType()


def test_hfa_read_elevation_units():

    ds = gdal.Open("../gcore/data/erdas_cm.img")
    unittype = ds.GetRasterBand(1).GetUnitType()
    assert unittype == "cm", "Failed to read elevation units"
    ds = None

    ds = gdal.Open("../gcore/data/erdas_feet.img")
    unittype = ds.GetRasterBand(1).GetUnitType()
    assert unittype == "feet", "Failed to read elevation units"
    ds = None

    ds = gdal.Open("../gcore/data/erdas_m.img")
    unittype = ds.GetRasterBand(1).GetUnitType()
    assert unittype == "meters", "Failed to read elevation units"
    ds = None


###############################################################################
#


def test_hfa_read_nan_nodata(tmp_vsimem):

    filename = tmp_vsimem / "test.img"
    ds = gdal.GetDriverByName("HFA").Create(filename, 1, 1, 1, gdal.GDT_Float32)
    ds.GetRasterBand(1).SetNoDataValue(float("nan"))
    ds.Close()

    with gdaltest.disable_exceptions():
        gdal.ErrorReset()
        with gdal.Open(filename) as ds:
            assert gdal.GetLastErrorMsg() == ""
            assert math.isnan(ds.GetRasterBand(1).GetNoDataValue())
