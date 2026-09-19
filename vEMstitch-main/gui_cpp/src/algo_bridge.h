#pragma once
// algo_bridge.h - the one place the upstream vEMstitch C++ headers are pulled in (engine layer;
// only engine.cpp includes it).
//
// Their sources in <root>\vEMstitch_c++ are compiled into the exe unmodified, all except their
// main.cpp (their command-line program), which needs the Linux-only dirent.h. engine.cpp does
// not call their three_stitching() (its rows race, see engine.cpp) but calls the building
// blocks declared here, in the same order their code does.
//
// Their include guards are mistyped (#ifndef _STITCHING_h_ / #define _STITCHING_H_, and the same
// in Utils.h), so the guard never takes effect: a second inclusion in the same translation unit
// redeclares default arguments and fails to compile. Include this header, never theirs directly.
//
// Requires /I"<root>\vEMstitch_c++\include" and the OpenCV include path. stitching.h pulls in
// omp.h, so their objects are built with /openmp; /DNDEBUG is required too, because without it
// OpenCV's CV_DbgAssert fires on their rigid_transform.cpp, which reads a CV_8U inlier mask with
// at<int>() (see README.md, Building).

#include "stitching.h"       // stitching_pair, stitching_rows (both overload pairs), omp.h

#include <string>
#include <tuple>

// Defined with external linkage in stitching.cpp (line 189) but declared in no header, so it is
// declared here to let engine.cpp call it. The signature must match theirs exactly, or the
// link fails. In "r" mode it measures the contrast of a strip half as wide as im2 at the right
// edge of im1 and at the left edge of im2: if either is nearly blank it joins the two images
// itself and returns (result, mass, false); otherwise it returns (empty, empty, true), meaning
// "run stitching_pair".
std::tuple<cv::Mat, cv::Mat, bool> preprocess(cv::Mat& im1, cv::Mat& im2, cv::Mat& im1_mask,
                                              cv::Mat& im2_mask, std::string& mode);
