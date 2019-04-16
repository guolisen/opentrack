/* Copyright (c) 2012 Patrick Ruoff
 * Copyright (c) 2014-2016 Stanislaw Halik <sthalik@misaki.pl>
 * Copyright (c) 2019 Stephane Lenclud
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "tracker-easy.h"
#include "video/video-widget.hpp"
#include "compat/math-imports.hpp"
#include "compat/check-visible.hpp"
#include "point-extractor.h"

#include <QHBoxLayout>
#include <QDebug>
#include <QFile>
#include <QCoreApplication>

#include <opencv2/calib3d.hpp>
#include <opencv2/highgui/highgui.hpp>

#include <iostream>

using namespace options;

// Disable debug
#define dbgout if (true) {} else std::cout
//#define infout if (true) {} else std::cout
// Enable debug
//#define dbgout if (false) {} else std::cout
#define infout if (false) {} else std::cout

namespace EasyTracker
{

    Tracker::Tracker() :
        iSettings{ KModuleName },
        iPreview{ preview_width, preview_height }
    {
        cv::setBreakOnError(true);
        cv::setNumThreads(1);

        connect(iSettings.b.get(), &bundle_::saving, this, &Tracker::maybe_reopen_camera, Qt::DirectConnection);
        connect(iSettings.b.get(), &bundle_::reloading, this, &Tracker::maybe_reopen_camera, Qt::DirectConnection);

        connect(&iSettings.fov, value_::value_changed<int>(), this, &Tracker::set_fov, Qt::DirectConnection);
        set_fov(iSettings.fov);

        CreateModelFromSettings();
    }

    Tracker::~Tracker()
    {
        //
        cv::destroyWindow("Preview");

        requestInterruption();
        wait();

        QMutexLocker l(&camera_mtx);
        camera->stop();        
    }


    // Compute Euler angles from rotation matrix
    void getEulerAngles(cv::Mat &rotCamerMatrix, cv::Vec3d &eulerAngles)
    {

        cv::Mat cameraMatrix, rotMatrix, transVect, rotMatrixX, rotMatrixY, rotMatrixZ;
        double* _r = rotCamerMatrix.ptr<double>();
        double projMatrix[12] = { _r[0],_r[1],_r[2],0,
                              _r[3],_r[4],_r[5],0,
                              _r[6],_r[7],_r[8],0 };

        cv::decomposeProjectionMatrix(cv::Mat(3, 4, CV_64FC1, projMatrix),
            cameraMatrix,
            rotMatrix,
            transVect,
            rotMatrixX,
            rotMatrixY,
            rotMatrixZ,
            eulerAngles);
    }

    ///
    void Tracker::CreateModelFromSettings()
    {
        // Construct the points defining the object we want to detect based on settings.
        // We are converting them from millimeters to centimeters.
        // TODO: Need to support clip too. That's cap only for now.
        // s.active_model_panel != PointModel::Clip
        iModel.clear();
        iModel.push_back(cv::Point3f(iSettings.cap_x / 10.0, iSettings.cap_z / 10.0, -iSettings.cap_y / 10.0)); // Right
        iModel.push_back(cv::Point3f(-iSettings.cap_x / 10.0, iSettings.cap_z / 10.0, -iSettings.cap_y / 10.0)); // Left
        iModel.push_back(cv::Point3f(0, 0, 0)); // Top
    }

    ///
    void Tracker::CreateCameraIntrinsicsMatrices()
    {
        // Create our camera matrix                
        iCameraMatrix.create(3, 3, CV_64FC1);
        iCameraMatrix.setTo(cv::Scalar(0));
        iCameraMatrix.at<double>(0, 0) = iCameraInfo.focalLengthX;
        iCameraMatrix.at<double>(1, 1) = iCameraInfo.focalLengthY;
        iCameraMatrix.at<double>(0, 2) = iCameraInfo.principalPointX;
        iCameraMatrix.at<double>(1, 2) = iCameraInfo.principalPointY;
        iCameraMatrix.at<double>(2, 2) = 1;

        // Create distortion cooefficients
        iDistCoeffsMatrix = cv::Mat::zeros(8, 1, CV_64FC1);
        // As per OpenCV docs they should be thus: k1, k2, p1, p2, k3, k4, k5, k6
        iDistCoeffsMatrix.at<double>(0, 0) = 0; // Radial first order
        iDistCoeffsMatrix.at<double>(1, 0) = iCameraInfo.radialDistortionSecondOrder; // Radial second order
        iDistCoeffsMatrix.at<double>(2, 0) = 0; // Tangential first order
        iDistCoeffsMatrix.at<double>(3, 0) = 0; // Tangential second order
        iDistCoeffsMatrix.at<double>(4, 0) = 0; // Radial third order
        iDistCoeffsMatrix.at<double>(5, 0) = iCameraInfo.radialDistortionFourthOrder; // Radial fourth order
        iDistCoeffsMatrix.at<double>(6, 0) = 0; // Radial fith order
        iDistCoeffsMatrix.at<double>(7, 0) = iCameraInfo.radialDistortionSixthOrder; // Radial sixth order
    }


    void Tracker::run()
    {
        maybe_reopen_camera();

        iFpsTimer.start();

        while (!isInterruptionRequested())
        {
            iTimer.start();

            bool new_frame = false;
            {
                QMutexLocker l(&camera_mtx);

                if (camera)
                {
                    std::tie(iFrame, new_frame) = camera->get_frame();
                }
                    
            }

            if (new_frame)
            {
                // Create OpenCV matrix from our frame
                // TODO: Assert channel size is one or two
                iMatFrame = cv::Mat(iFrame.height, iFrame.width, CV_MAKETYPE((iFrame.channelSize == 2 ? CV_16U : CV_8U), iFrame.channels), iFrame.data, iFrame.stride);
                iFrameCount++;

                const bool preview_visible = check_is_visible();
                if (preview_visible)
                {
                    iPreview = iMatFrame;
                }

                iPoints.clear();
                iPointExtractor.ExtractPoints(iMatFrame, (preview_visible ? &iPreview.iFrameRgb : nullptr), iPoints);                

                const bool success = iPoints.size() >= KPointCount;

                int topPointIndex = -1;

                {
                    QMutexLocker l(&center_lock);

                    if (success)
                    {
                        ever_success.store(true, std::memory_order_relaxed);

                        // Solve P3P problem with OpenCV
                    
                        //Bitmap origin is top left
                        iTrackedPoints.clear();
                        // Tracked points must match the order of the object model points.
                        // Find top most point, that's the one with min Y as we assume our guy's head is not up side down
                        int minY = std::numeric_limits<int>::max();
                        for (int i = 0; i < 3; i++)
                        {
                            if (iPoints[i].y < minY)
                            {
                                minY = iPoints[i].y;
                                topPointIndex = i;
                            }
                        }

                        int rightPointIndex = -1;
                        int maxX = 0;

                        // Find right most point 
                        for (int i = 0; i < 3; i++)
                        {
                            // Excluding top most point
                            if (i != topPointIndex && iPoints[i].x > maxX)
                            {
                                maxX = iPoints[i].x;
                                rightPointIndex = i;
                            }
                        }

                        // Find left most point
                        int leftPointIndex = -1;
                        for (int i = 0; i < 3; i++)
                        {
                            // Excluding top most point
                            if (i != topPointIndex && i != rightPointIndex)
                            {
                                leftPointIndex = i;
                                break;
                            }
                        }

                        //
                        iTrackedPoints.push_back(iPoints[rightPointIndex]);
                        iTrackedPoints.push_back(iPoints[leftPointIndex]);
                        iTrackedPoints.push_back(iPoints[topPointIndex]);

                        dbgout << "Object: " << iModel << "\n";
                        dbgout << "Points: " << iTrackedPoints << "\n";


                        // TODO: try SOLVEPNP_AP3P too, make it a settings option?
                        iAngles.clear();
                        iBestSolutionIndex = -1;
                        int solutionCount = cv::solveP3P(iModel, iTrackedPoints, iCameraMatrix, iDistCoeffsMatrix, iRotations, iTranslations, cv::SOLVEPNP_P3P);

                        if (solutionCount > 0)
                        {
                            dbgout << "Solution count: " << solutionCount << "\n";
                            int minPitch = std::numeric_limits<int>::max();
                            // Find the solution we want amongst all possible ones
                            for (int i = 0; i < solutionCount; i++)
                            {
                                dbgout << "Translation:\n";
                                dbgout << iTranslations.at(i);
                                dbgout << "\n";
                                dbgout << "Rotation:\n";
                                //dbgout << rvecs.at(i);
                                cv::Mat rotationCameraMatrix;
                                cv::Rodrigues(iRotations[i], rotationCameraMatrix);
                                cv::Vec3d angles;
                                getEulerAngles(rotationCameraMatrix, angles);
                                iAngles.push_back(angles);

                                // Check if pitch is closest to zero
                                int absolutePitch = std::abs(angles[0]);
                                if (minPitch > absolutePitch)
                                {
                                    // The solution with pitch closest to zero is the one we want
                                    minPitch = absolutePitch;
                                    iBestSolutionIndex = i;
                                }

                                dbgout << angles;
                                dbgout << "\n";
                            }

                            dbgout << "\n";
                        }
                    }

                    // Send solution data back to main thread
                    QMutexLocker l2(&data_lock);
                    if (iBestSolutionIndex != -1)
                    {
                        iBestAngles = iAngles[iBestSolutionIndex];
                        iBestTranslation = iTranslations[iBestSolutionIndex];
                    }

                }

                if (preview_visible)
                {
                    std::ostringstream ss;
                    ss << "FPS: " << iFps << "/" << iSkippedFps;
                    iPreview.DrawInfo(ss.str());

                    //
                    if (topPointIndex != -1)
                    {
                        // Render a cross to indicate which point is the head
                        iPreview.DrawCross(iPoints[topPointIndex]);
                    }

                    // Show full size preview pop-up
                    if (iSettings.debug)
                    {
                        cv::imshow("Preview", iPreview.iFrameRgb);
                        cv::waitKey(1);
                    }

                    // Update preview widget
                    widget->update_image(iPreview.get_bitmap());

                    auto[w, h] = widget->preview_size();
                    if (w != preview_width || h != preview_height)
                    {
                        // Resize preivew if widget size has changed
                        preview_width = w; preview_height = h;
                        iPreview = Preview(w, h);
                    }
                }
                else
                {
                    // No preview, destroy preview pop-up
                    if (iSettings.debug)
                    {
                        cv::destroyWindow("Preview");
                    }                    
                }

                dbgout << "Frame time:" << iTimer.elapsed_seconds() << "\n";
            }
            else
            {
                iSkippedFrameCount++;
            }

            // Pace ourselves, drastically reduce CPU usage
            // TODO: Consider using QTimer instead of QThread
            msleep(1000 / 55);

            // Compute FPS
            double elapsed = iFpsTimer.elapsed_seconds();
            if (elapsed >= 1.0)
            {
                iFps = iFrameCount / elapsed;
                iSkippedFps = iSkippedFrameCount / elapsed;
                iFrameCount = 0;
                iSkippedFrameCount = 0;
                iFpsTimer.start();
            }
        }
    }

    bool Tracker::maybe_reopen_camera()
    {
        QMutexLocker l(&camera_mtx);

        if (camera->is_open())
        {
            return true;
        }

        iCameraInfo.fps = iSettings.cam_fps;
        iCameraInfo.width = iSettings.cam_res_x;
        iCameraInfo.height = iSettings.cam_res_y;

        bool res = camera->start(iCameraInfo);
        // We got new our camera intrinsics, create corresponding matrices
        CreateCameraIntrinsicsMatrices();
        return res;
    }

    void Tracker::set_fov(int value)
    {
        QMutexLocker l(&camera_mtx);

    }

    module_status Tracker::start_tracker(QFrame* video_frame)
    {
        //video_frame->setAttribute(Qt::WA_NativeWindow);

        widget = std::make_unique<video_widget>(video_frame);
        layout = std::make_unique<QHBoxLayout>(video_frame);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(widget.get());
        video_frame->setLayout(layout.get());
        //video_widget->resize(video_frame->width(), video_frame->height());
        video_frame->show();

        // Create our camera
        camera = video::make_camera(iSettings.camera_name);

        start(QThread::HighPriority);

        return {};
    }

    void Tracker::data(double *data)
    {
        if (ever_success.load(std::memory_order_relaxed))
        {
            // Get data back from tracker thread
            QMutexLocker l(&data_lock);
            data[Yaw] = iBestAngles[1];
            data[Pitch] = iBestAngles[0];
            data[Roll] = iBestAngles[2];
            data[TX] = iBestTranslation[0];
            data[TY] = iBestTranslation[1];
            data[TZ] = iBestTranslation[2];
        }
    }

    bool Tracker::center()
    {
        QMutexLocker l(&center_lock);
        //TODO: Do we need to do anything there?
        return false;
    }


}
