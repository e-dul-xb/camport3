#include "../common/common.hpp"

#include <chrono>

#include <ros/ros.h>
#include <sensor_msgs/Image.h>
//#include <sensor_msgs/PointCloud.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>

#include <boost/program_options.hpp>


void eventCallback(TY_EVENT_INFO *event_info, void *userdata)
{
    if (event_info->eventId == TY_EVENT_DEVICE_OFFLINE) {
        LOGD("=== Event Callback: Device Offline!");
        // Note: 
        //     Please set TY_BOOL_KEEP_ALIVE_ONOFF feature to false if you need to debug with breakpoint!
    }
    else if (event_info->eventId == TY_EVENT_LICENSE_ERROR) {
        LOGD("=== Event Callback: License Error!");
    }
}

int main(int argc, char* argv[])
{
    ros::init(argc, argv, "percipio_fetch_frame");
    ros::NodeHandle n;

    ros::Publisher ir_l_pub = n.advertise<sensor_msgs::Image>("/base_depth/ir/left/image", 10);
    ros::Publisher ir_r_pub = n.advertise<sensor_msgs::Image>("/base_depth/ir/right/image", 10);


    std::string ID, IP;
    TY_INTERFACE_HANDLE hIface = NULL;
    TY_ISP_HANDLE hColorIspHandle = NULL;
    TY_DEV_HANDLE hDevice = NULL;
    int32_t color, ir, depth;
    color = ir = depth = 1;

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-id") == 0){
            ID = argv[++i];
        } else if(strcmp(argv[i], "-ip") == 0) {
            IP = argv[++i];
        } else if(strcmp(argv[i], "-color=off") == 0) {
            color = 0;
        } else if(strcmp(argv[i], "-depth=off") == 0) {
            depth = 0;
        } else if(strcmp(argv[i], "-ir=off") == 0) {
            ir = 0;
        } else if(strcmp(argv[i], "-h") == 0) {
            LOGI("Usage: SimpleView_FetchFrame [-h] [-id <ID>] [-ip <IP>]");
            return 0;
        }
    }

    if (!color && !depth && !ir) {
        LOGD("At least one component need to be on");
        return -1;
    }

    LOGD("Init lib");
    ASSERT_OK( TYInitLib() );
    TY_VERSION_INFO ver;
    ASSERT_OK( TYLibVersion(&ver) );
    LOGD("     - lib version: %d.%d.%d", ver.major, ver.minor, ver.patch);

    std::vector<TY_DEVICE_BASE_INFO> selected;
    ASSERT_OK( selectDevice(TY_INTERFACE_ALL, ID, IP, 1, selected) );
    ASSERT(selected.size() > 0);
    TY_DEVICE_BASE_INFO& selectedDev = selected[0];

    ASSERT_OK( TYOpenInterface(selectedDev.iface.id, &hIface) );
    ASSERT_OK( TYOpenDevice(hIface, selectedDev.id, &hDevice) );

    int32_t allComps;
    ASSERT_OK( TYGetComponentIDs(hDevice, &allComps) );

    ///try to enable color camera
    if(allComps & TY_COMPONENT_RGB_CAM  && color) {
        LOGD("Has RGB camera, open RGB cam");
        ASSERT_OK( TYEnableComponents(hDevice, TY_COMPONENT_RGB_CAM) );
        //create a isp handle to convert raw image(color bayer format) to rgb image
        ASSERT_OK(TYISPCreate(&hColorIspHandle));
        //Init code can be modified in common.hpp
        //NOTE: Should set RGB image format & size before init ISP
        ASSERT_OK(ColorIspInitSetting(hColorIspHandle, hDevice));
        //You can  call follow function to show  color isp supported features
#if 0
        ColorIspShowSupportedFeatures(hColorIspHandle);
#endif
        //You can turn on auto exposure function as follow ,but frame rate may reduce .
        //Device may be casually stucked  1~2 seconds while software is trying to adjust device exposure time value
#if 0
        ASSERT_OK(ColorIspInitAutoExposure(hColorIspHandle, hDevice));
#endif
    }

    if (allComps & TY_COMPONENT_IR_CAM_LEFT && ir) {
        LOGD("Has IR left camera, open IR left cam");
        ASSERT_OK(TYEnableComponents(hDevice, TY_COMPONENT_IR_CAM_LEFT));
    }

    if (allComps & TY_COMPONENT_IR_CAM_RIGHT && ir) {
        LOGD("Has IR right camera, open IR right cam");
		    ASSERT_OK(TYEnableComponents(hDevice, TY_COMPONENT_IR_CAM_RIGHT));
    }

    //try to enable depth map
    LOGD("Configure components, open depth cam");
    DepthViewer depthViewer("Depth");
    if (allComps & TY_COMPONENT_DEPTH_CAM && depth) {
        std::vector<TY_ENUM_ENTRY> image_mode_list;
        ASSERT_OK(get_feature_enum_list(hDevice, TY_COMPONENT_DEPTH_CAM, TY_ENUM_IMAGE_MODE, image_mode_list));
        for (int idx = 0; idx < image_mode_list.size(); idx++){
            TY_ENUM_ENTRY &entry = image_mode_list[idx];
            //try to select a VGA resolution
            if (TYImageWidth(entry.value) == 640 || TYImageHeight(entry.value) == 640){
                LOGD("Select Depth Image Mode: %s", entry.description);
                int err = TYSetEnum(hDevice, TY_COMPONENT_DEPTH_CAM, TY_ENUM_IMAGE_MODE, entry.value);
                ASSERT(err == TY_STATUS_OK || err == TY_STATUS_NOT_PERMITTED);
                break;
            }
        }
        ASSERT_OK(TYEnableComponents(hDevice, TY_COMPONENT_DEPTH_CAM));
        //depth map pixel format is uint16_t ,which default unit is  1 mm
        //the acutal depth (mm)= PxielValue * ScaleUnit 
        float scale_unit = 1.;
        TYGetFloat(hDevice, TY_COMPONENT_DEPTH_CAM, TY_FLOAT_SCALE_UNIT, &scale_unit);
        depthViewer.depth_scale_unit = scale_unit;
    }



    LOGD("Prepare image buffer");
    uint32_t frameSize;
    ASSERT_OK( TYGetFrameBufferSize(hDevice, &frameSize) );
    LOGD("     - Get size of framebuffer, %d", frameSize);

    LOGD("     - Allocate & enqueue buffers");
    char* frameBuffer[2];
    frameBuffer[0] = new char[frameSize];
    frameBuffer[1] = new char[frameSize];
    LOGD("     - Enqueue buffer (%p, %d)", frameBuffer[0], frameSize);
    ASSERT_OK( TYEnqueueBuffer(hDevice, frameBuffer[0], frameSize) );
    LOGD("     - Enqueue buffer (%p, %d)", frameBuffer[1], frameSize);
    ASSERT_OK( TYEnqueueBuffer(hDevice, frameBuffer[1], frameSize) );

    LOGD("Register event callback");
    ASSERT_OK(TYRegisterEventCallback(hDevice, eventCallback, NULL));

    bool hasTrigger;
    ASSERT_OK(TYHasFeature(hDevice, TY_COMPONENT_DEVICE, TY_STRUCT_TRIGGER_PARAM, &hasTrigger));
    if (hasTrigger) {
        LOGD("Disable trigger mode");
        TY_TRIGGER_PARAM trigger;
        trigger.mode = TY_TRIGGER_MODE_OFF;
        ASSERT_OK(TYSetStruct(hDevice, TY_COMPONENT_DEVICE, TY_STRUCT_TRIGGER_PARAM, &trigger, sizeof(trigger)));
    }

    LOGD("Start capture");
    ASSERT_OK( TYStartCapture(hDevice) );

    LOGD("While loop to fetch frame");
    bool exit_main = false;
    TY_FRAME_DATA frame;
    int index = 0;
    uint64_t ir_l_offset_stamp_us = 0;
    uint64_t ir_r_offset_stamp_us = 0;
    while(!exit_main && ros::ok()) {
        int err = TYFetchFrame(hDevice, &frame, -1);
        if( err == TY_STATUS_OK ) {
            LOGD("Get frame %d", ++index);

            int fps = get_fps();
            if (fps > 0){
                LOGI("fps: %d", fps);
            }

            cv::Mat depth, irl, irr, color;
            parseFrame(frame, &depth, &irl, &irr, &color, hColorIspHandle);
            if(!depth.empty()){
                depthViewer.show(depth);
            }
            if(!irl.empty()){
                cv_bridge::CvImage cvi_mat_ir;
                cvi_mat_ir.encoding = sensor_msgs::image_encodings::MONO8;
                cvi_mat_ir.image = irl;
                sensor_msgs::ImagePtr msg_raw = cvi_mat_ir.toImageMsg();
                uint64_t stamp_us = 0;
                parseComponentTiestamp(frame, TY_COMPONENT_IR_CAM_LEFT, stamp_us);
                if(ir_l_offset_stamp_us == 0){
                  // reference first timestamp
                  ir_l_offset_stamp_us = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count() * 1000 - stamp_us;
                }
                stamp_us += ir_l_offset_stamp_us;
                msg_raw->header.stamp.sec = stamp_us / 1000000;
                msg_raw->header.stamp.nsec = (stamp_us - msg_raw->header.stamp.sec * 1000000) * 1000;
                ir_l_pub.publish(msg_raw);
                cv::imshow("LeftIR", irl);
            }
            if(!irr.empty()){
                cv_bridge::CvImage cvi_mat_ir;
                cvi_mat_ir.encoding = sensor_msgs::image_encodings::MONO8;
                cvi_mat_ir.image = irr;
                sensor_msgs::ImagePtr msg_raw = cvi_mat_ir.toImageMsg();
                uint64_t stamp_us = 0;
                parseComponentTiestamp(frame, TY_COMPONENT_IR_CAM_RIGHT, stamp_us);
                if(ir_r_offset_stamp_us == 0){
                  // reference first timestamp
                  ir_r_offset_stamp_us = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count() * 1000 - stamp_us;
                }
                stamp_us += ir_r_offset_stamp_us;
                msg_raw->header.stamp.sec = stamp_us / 1000000;
                msg_raw->header.stamp.nsec = (stamp_us - msg_raw->header.stamp.sec * 1000000) * 1000;
                ir_r_pub.publish(msg_raw);
                cv::imshow("RightIR", irr);
            }
            if(!color.empty()){ cv::imshow("Color", color); }

            int key = cv::waitKey(1);
            switch(key & 0xff) {
            case 0xff:
                break;
            case 'q':
                exit_main = true;
                break;
            default:
                LOGD("Unmapped key %d", key);
            }

            TYISPUpdateDevice(hColorIspHandle);
            LOGD("Re-enqueue buffer(%p, %d)"
                , frame.userBuffer, frame.bufferSize);
            ASSERT_OK( TYEnqueueBuffer(hDevice, frame.userBuffer, frame.bufferSize) );
        }
    }
    ASSERT_OK( TYStopCapture(hDevice) );
    ASSERT_OK( TYCloseDevice(hDevice) );
    ASSERT_OK( TYCloseInterface(hIface) );
    ASSERT_OK(TYISPRelease(&hColorIspHandle));
    ASSERT_OK( TYDeinitLib() );
    delete frameBuffer[0];
    delete frameBuffer[1];

    LOGD("Main done!");
    return 0;
}
