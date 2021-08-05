/*
 * Tencent is pleased to support the open source community by making IoT Hub
 available.
 * Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

 * Licensed under the MIT License (the "License"); you may not use this file
 except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software
 distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 KIND,
 * either express or implied. See the License for the specific language
 governing permissions and
 * limitations under the License.
 *
 */

#include "lite-utils.h"
#include "qcloud_iot_export.h"
#include "qcloud_iot_import.h"
#include "utils_timer.h"
#include "utils_getopt.h"

#define DEMO_ASR_FILE     0
#define DEMO_ASR_REALTIEM 1
#define DEMO_ASR_SENTENCE 2
#define DEMO_ASR          DEMO_ASR_SENTENCE
#define PER_SLICE_SIZE    (6400)  // 200ms/16K/16bit ~ 6.4KB

#ifdef AUTH_MODE_CERT
static char sg_cert_file[PATH_MAX + 1];  // full path of device cert file
static char sg_key_file[PATH_MAX + 1];   // full path of device key file
#endif

static MQTTEventType sg_subscribe_event_result = MQTT_EVENT_UNDEF;
static bool          sg_control_msg_arrived    = false;
static char          sg_data_report_buffer[2048];
static size_t        sg_data_report_buffersize = sizeof(sg_data_report_buffer) / sizeof(sg_data_report_buffer[0]);

/*data_config.c can be generated by tools/codegen.py -c product.json*/
/*-----------------data config start  -------------------*/
#define TOTAL_PROPERTY_COUNT 1

static sDataPoint sg_DataTemplate[TOTAL_PROPERTY_COUNT];

typedef struct _ProductDataDefine {
    int m_test;
} ProductDataDefine;

static ProductDataDefine sg_ProductData;
static DeviceInfo        sg_DeviceInfo;

static void _init_data_template(void)
{
    sg_ProductData.m_test                 = 0;
    sg_DataTemplate[0].data_property.data = &sg_ProductData.m_test;
    sg_DataTemplate[0].data_property.key  = "test";
    sg_DataTemplate[0].data_property.type = TYPE_TEMPLATE_INT;
    sg_DataTemplate[0].state              = eNOCHANGE;
};
/*-----------------data config end  -------------------*/

static void event_handler(void *pclient, void *handle_context, MQTTEventMsg *msg)
{
    uintptr_t packet_id = (uintptr_t)msg->msg;

    switch (msg->event_type) {
        case MQTT_EVENT_UNDEF:
            Log_i("undefined event occur.");
            break;

        case MQTT_EVENT_DISCONNECT:
            Log_i("MQTT disconnect.");
            break;

        case MQTT_EVENT_RECONNECT:
            Log_i("MQTT reconnect.");
            break;

        case MQTT_EVENT_SUBCRIBE_SUCCESS:
            sg_subscribe_event_result = msg->event_type;
            Log_i("subscribe success, packet-id=%u", (unsigned int)packet_id);
            break;

        case MQTT_EVENT_SUBCRIBE_TIMEOUT:
            sg_subscribe_event_result = msg->event_type;
            Log_i("subscribe wait ack timeout, packet-id=%u", (unsigned int)packet_id);
            break;

        case MQTT_EVENT_SUBCRIBE_NACK:
            sg_subscribe_event_result = msg->event_type;
            Log_i("subscribe nack, packet-id=%u", (unsigned int)packet_id);
            break;

        case MQTT_EVENT_PUBLISH_SUCCESS:
            Log_d("publish success, packet-id=%u", (unsigned int)packet_id);
            break;

        case MQTT_EVENT_PUBLISH_TIMEOUT:
            Log_i("publish timeout, packet-id=%u", (unsigned int)packet_id);
            break;

        case MQTT_EVENT_PUBLISH_NACK:
            Log_i("publish nack, packet-id=%u", (unsigned int)packet_id);
            break;
        default:
            Log_i("Should NOT arrive here.");
            break;
    }
}

/*add user init code, like sensor init*/
static void _usr_init(void)
{
    Log_d("add your init code here");
}

// Setup MQTT construct parameters
static int _setup_connect_init_params(TemplateInitParams *initParams, DeviceInfo *device_info)
{
    initParams->region      = device_info->region;
    initParams->device_name = device_info->device_name;
    initParams->product_id  = device_info->product_id;

#ifdef AUTH_MODE_CERT
    /* TLS with certs*/
    char  certs_dir[PATH_MAX + 1] = "certs";
    char  current_path[PATH_MAX + 1];
    char *cwd = getcwd(current_path, sizeof(current_path));
    if (cwd == NULL) {
        Log_e("getcwd return NULL");
        return QCLOUD_ERR_FAILURE;
    }
    sprintf(sg_cert_file, "%s/%s/%s", current_path, certs_dir, device_info->dev_cert_file_name);
    sprintf(sg_key_file, "%s/%s/%s", current_path, certs_dir, device_info->dev_key_file_name);

    initParams->cert_file = sg_cert_file;
    initParams->key_file  = sg_key_file;
#else
    initParams->device_secret = device_info->device_secret;
#endif

    initParams->command_timeout        = QCLOUD_IOT_MQTT_COMMAND_TIMEOUT;
    initParams->keep_alive_interval_ms = QCLOUD_IOT_MQTT_KEEP_ALIVE_INTERNAL;
    initParams->auto_connect_enable    = 1;
    initParams->event_handle.h_fp      = event_handler;
    initParams->usr_control_handle     = NULL;

    return QCLOUD_RET_SUCCESS;
}

#ifdef LOG_UPLOAD
// init log upload module
static int _init_log_upload(TemplateInitParams *init_params)
{
    LogUploadInitParams log_init_params;
    memset(&log_init_params, 0, sizeof(LogUploadInitParams));

    log_init_params.region      = init_params->region;
    log_init_params.product_id  = init_params->product_id;
    log_init_params.device_name = init_params->device_name;
#ifdef AUTH_MODE_CERT
    log_init_params.sign_key = init_params->cert_file;
#else
    log_init_params.sign_key = init_params->device_secret;
#endif

#if defined(__linux__) || defined(WIN32)
    log_init_params.read_func     = HAL_Log_Read;
    log_init_params.save_func     = HAL_Log_Save;
    log_init_params.del_func      = HAL_Log_Del;
    log_init_params.get_size_func = HAL_Log_Get_Size;
#endif

    return IOT_Log_Init_Uploader(&log_init_params);
}
#endif

/*control msg from server will trigger this callback*/
static void OnControlMsgCallback(void *pClient, const char *pJsonValueBuffer, uint32_t valueLength,
                                 DeviceProperty *pProperty)
{
    int i = 0;

    for (i = 0; i < TOTAL_PROPERTY_COUNT; i++) {
        /* handle self defined string/json here. Other properties are dealed in _handle_delta()*/
        if (strcmp(sg_DataTemplate[i].data_property.key, pProperty->key) == 0) {
            sg_DataTemplate[i].state = eCHANGED;
            Log_d("Property=%s changed", pProperty->key);
            sg_control_msg_arrived = true;
            return;
        }
    }

    Log_e("Property=%s changed no match", pProperty->key);
}

static void OnReportReplyCallback(void *pClient, Method method, ReplyAck replyAck, const char *pJsonDocument,
                                  void *pUserdata)
{
    Log_i("recv report_reply(ack=%d): %s", replyAck, pJsonDocument);
}

// register data template properties
static int _register_data_template_property(void *pTemplate_client)
{
    int i, rc;

    for (i = 0; i < TOTAL_PROPERTY_COUNT; i++) {
        rc = IOT_Template_Register_Property(pTemplate_client, &sg_DataTemplate[i].data_property, OnControlMsgCallback);
        if (rc != QCLOUD_RET_SUCCESS) {
            rc = IOT_Template_Destroy(pTemplate_client);
            Log_e("register device data template property failed, err: %d", rc);
            return rc;
        } else {
            Log_i("data template property=%s registered.", sg_DataTemplate[i].data_property.key);
        }
    }

    return QCLOUD_RET_SUCCESS;
}

/*You should get the real info for your device, here just for example*/
static int _get_sys_info(void *handle, char *pJsonDoc, size_t sizeOfBuffer)
{
    /*platform info has at least one of module_hardinfo/module_softinfo/fw_ver
     * property*/
    DeviceProperty plat_info[] = {
        {.key = "module_hardinfo", .type = TYPE_TEMPLATE_STRING, .data = "ESP8266"},
        {.key = "module_softinfo", .type = TYPE_TEMPLATE_STRING, .data = "V1.0"},
        {.key = "fw_ver", .type = TYPE_TEMPLATE_STRING, .data = QCLOUD_IOT_DEVICE_SDK_VERSION},
        {.key = "imei", .type = TYPE_TEMPLATE_STRING, .data = "11-22-33-44"},
        {.key = "lat", .type = TYPE_TEMPLATE_STRING, .data = "22.546015"},
        {.key = "lon", .type = TYPE_TEMPLATE_STRING, .data = "113.941125"},
        {.key = NULL, .data = NULL}  // end
    };

    /*self define info*/
    DeviceProperty self_info[] = {
        {.key = "append_info", .type = TYPE_TEMPLATE_STRING, .data = "your self define info"},
        {.key = NULL, .data = NULL}  // end
    };

    return IOT_Template_JSON_ConstructSysInfo(handle, pJsonDoc, sizeOfBuffer, plat_info, self_info);
}

/*get property state, changed or not*/
static eDataState get_property_state(void *pProperyData)
{
    int i;

    for (i = 0; i < TOTAL_PROPERTY_COUNT; i++) {
        if (sg_DataTemplate[i].data_property.data == pProperyData) {
            return sg_DataTemplate[i].state;
        }
    }

    Log_e("no property matched");
    return eNOCHANGE;
}

/*set property state, changed or no change*/
static void set_property_state(void *pProperyData, eDataState state)
{
    int i;

    for (i = 0; i < TOTAL_PROPERTY_COUNT; i++) {
        if (sg_DataTemplate[i].data_property.data == pProperyData) {
            sg_DataTemplate[i].state = state;
            break;
        }
    }
}

/*find propery need report*/
static int find_wait_report_property(DeviceProperty *pReportDataList[])
{
    int i, j;

    for (i = 0, j = 0; i < TOTAL_PROPERTY_COUNT; i++) {
        if (!strcmp(sg_DataTemplate[i].data_property.key, "asr_response")) {
            continue;
        }

        if (eCHANGED == sg_DataTemplate[i].state) {
            pReportDataList[j++]     = &(sg_DataTemplate[i].data_property);
            sg_DataTemplate[i].state = eNOCHANGE;
        }
    }

    return j;
}

static int _resource_event_usr_cb(void *pContext, const char *msg, uint32_t msgLen, int event)
{
    int ret = QCLOUD_RET_SUCCESS;
    Log_d("resource event %d", (IOT_FILE_UsrEvent)event);

    return ret;
}

/*get local property data, like sensor data*/
static void _refresh_local_property(void) {}

static void deal_down_stream_user_logic(void *client, ProductDataDefine *asr)
{
    if (eCHANGED == get_property_state(&asr->m_test)) {
        Log_d("someting about your own product logic wait to be done");
        set_property_state(&asr->m_test, eNOCHANGE);
    }
}

/* demo for up-stream code */
static int deal_up_stream_user_logic(DeviceProperty *pReportDataList[], int *pCount)
{
    // refresh local property
    _refresh_local_property();

    /*find propery need report*/
    *pCount = find_wait_report_property(pReportDataList);

    return (*pCount > 0) ? QCLOUD_RET_SUCCESS : QCLOUD_ERR_FAILURE;
}

static void asr_result_cb(uint32_t request_id, char *res_text, int total_resutl_num, int resutl_seq)
{
    Log_i("request_id:%d: %d/%d text:%s", request_id, resutl_seq, total_resutl_num, res_text);
}

static int parse_arguments(int argc, char **argv)
{
    int c;
    while ((c = utils_getopt(argc, argv, "c:l:")) != EOF) switch (c) {
            case 'c':
                if (HAL_SetDevInfoFile(utils_optarg))
                    return -1;
                break;

            default:
                HAL_Printf(
                    "usage: %s [options]\n"
                    "  [-c <config file for DeviceInfo>] \n",
                    argv[0]);
                return -1;
        }
    return 0;
}

int main(int argc, char **argv)
{
    DeviceProperty *pReportDataList[TOTAL_PROPERTY_COUNT];
    sReplyPara      replyPara;
    int             ReportCont;
    int             rc;
    void *          data_template_client = NULL;
    void *          asr_client           = NULL;

    // init log level
    IOT_Log_Set_Level(eLOG_DEBUG);
    // parse arguments for device info file
    rc = parse_arguments(argc, argv);
    if (rc != QCLOUD_RET_SUCCESS) {
        Log_e("parse arguments error, rc = %d", rc);
        return rc;
    }

    rc = HAL_GetDevInfo(&sg_DeviceInfo);
    if (QCLOUD_RET_SUCCESS != rc) {
        Log_e("get device info failed: %d", rc);
        return rc;
    }

    // init connection
    TemplateInitParams init_params = DEFAULT_TEMPLATE_INIT_PARAMS;
    rc                             = _setup_connect_init_params(&init_params, &sg_DeviceInfo);
    if (rc != QCLOUD_RET_SUCCESS) {
        Log_e("init params err,rc=%d", rc);
        return rc;
    }

#ifdef LOG_UPLOAD
    // _init_log_upload should be done after _setup_connect_init_params and before IOT_Template_Construct
    rc = _init_log_upload(&init_params);
    if (rc != QCLOUD_RET_SUCCESS) {
        Log_e("init log upload error, rc = %d", rc);
    }
#endif

    data_template_client = IOT_Template_Construct(&init_params, NULL);
    if (data_template_client != NULL) {
        Log_i("Cloud Device Construct Success");
    } else {
        Log_e("Cloud Device Construct Failed");
        return QCLOUD_ERR_FAILURE;
    }

    // usr init
    _usr_init();

    // init data template
    _init_data_template();

    // register data template propertys here
    rc = _register_data_template_property(data_template_client);
    if (rc == QCLOUD_RET_SUCCESS) {
        Log_i("Register data template propertys Success");
    } else {
        Log_e("Register data template propertys Failed: %d", rc);
        goto exit;
    }

    // report device info, then you can manager your product by these info, like position
    rc = _get_sys_info(data_template_client, sg_data_report_buffer, sg_data_report_buffersize);
    if (QCLOUD_RET_SUCCESS == rc) {
        rc = IOT_Template_Report_SysInfo_Sync(data_template_client, sg_data_report_buffer, sg_data_report_buffersize,
                                              QCLOUD_IOT_MQTT_COMMAND_TIMEOUT);
        if (rc != QCLOUD_RET_SUCCESS) {
            Log_e("Report system info fail, err: %d", rc);
        }
    } else {
        Log_e("Get system info fail, err: %d", rc);
    }

    // get the property changed during offline
    rc = IOT_Template_GetStatus_sync(data_template_client, QCLOUD_IOT_MQTT_COMMAND_TIMEOUT);
    if (rc != QCLOUD_RET_SUCCESS) {
        Log_e("Get data status fail, err: %d", rc);
    } else {
        Log_d("Get data status success");
    }

    asr_client =
        IOT_Asr_Init(sg_DeviceInfo.product_id, sg_DeviceInfo.device_name, data_template_client, _resource_event_usr_cb);
    if (asr_client == NULL) {
        Log_e("Asr client Construct Failed");
        goto exit;
    }

#ifdef MULTITHREAD_ENABLED
    if (QCLOUD_RET_SUCCESS != IOT_Template_Start_Yield_Thread(data_template_client)) {
        Log_e("start template yield thread fail");
        goto exit;
    }
#endif

    while (IOT_Template_IsConnected(data_template_client) || rc == QCLOUD_ERR_MQTT_ATTEMPTING_RECONNECT ||
           rc == QCLOUD_RET_MQTT_RECONNECTED || QCLOUD_RET_SUCCESS == rc) {
        rc = IOT_Template_Yield(data_template_client, 200);
        if (rc == QCLOUD_ERR_MQTT_ATTEMPTING_RECONNECT) {
            HAL_SleepMs(1000);
            continue;
        } else if (rc != QCLOUD_RET_SUCCESS && rc != QCLOUD_RET_MQTT_RECONNECTED) {
            Log_e("Exit loop caused of errCode: %d", rc);
            break;
        }

        /* handle control msg from server */
        if (sg_control_msg_arrived) {
            deal_down_stream_user_logic(data_template_client, &sg_ProductData);
            /* control msg should reply, otherwise server treat device didn't receive
             * and retain the msg which would be get by get status*/
            memset((char *)&replyPara, 0, sizeof(sReplyPara));
            replyPara.code          = eDEAL_SUCCESS;
            replyPara.timeout_ms    = QCLOUD_IOT_MQTT_COMMAND_TIMEOUT;
            replyPara.status_msg[0] = '\0';  // add extra info to replyPara.status_msg when error occured

            rc = IOT_Template_ControlReply(data_template_client, sg_data_report_buffer, sg_data_report_buffersize,
                                           &replyPara);
            if (rc == QCLOUD_RET_SUCCESS) {
                Log_d("Contol msg reply success");
                sg_control_msg_arrived = false;
            } else {
                Log_e("Contol msg reply failed, err: %d", rc);
            }
        }

        /*report msg to server report the lastest properties's status*/
        if (QCLOUD_RET_SUCCESS == deal_up_stream_user_logic(pReportDataList, &ReportCont)) {
            rc = IOT_Template_JSON_ConstructReportArray(data_template_client, sg_data_report_buffer,
                                                        sg_data_report_buffersize, ReportCont, pReportDataList);
            if (rc == QCLOUD_RET_SUCCESS) {
                rc = IOT_Template_Report(data_template_client, sg_data_report_buffer, sg_data_report_buffersize,
                                         OnReportReplyCallback, NULL, QCLOUD_IOT_MQTT_COMMAND_TIMEOUT);
                if (rc == QCLOUD_RET_SUCCESS) {
                    Log_i("data template reporte success");
                } else {
                    Log_e("data template reporte failed, err: %d", rc);
                }
            } else {
                Log_e("construct reporte data failed, err: %d", rc);
            }
        }

        /*Demo usagae for ASR ---------> begin */
        if (DEMO_ASR == DEMO_ASR_FILE) {
            // asr record file requst, sg_ProductData.asr_response from OnControlMsgCallback is asr result
            RecordAsrConf config      = {0};
            config.request_timeout_ms = 10000;
            config.req_type           = eASR_FILE;
            config.ch_num             = 1;
            config.engine_type        = eENGINE_16K_ZH;
            rc = IOT_Asr_RecordFile_Request(asr_client, "./test_file/test.wav", &config, asr_result_cb);
            if (rc > 0) {
                Log_i("record file %s's request_id %d ", "test.wav", rc);
            } else {
                Log_e("record file %s's asr request fail, err: %d", "test.wav", rc);
            }
            HAL_SleepMs(1000);  // response time depends on file size
        }

        if (DEMO_ASR == DEMO_ASR_SENTENCE) {
            // asr record file requst, sg_ProductData.asr_response from OnControlMsgCallback is asr result
            RecordAsrConf config      = {0};
            config.request_timeout_ms = 10000;
            config.req_type           = eASR_SENTENCE;
            config.ch_num             = 1;
            config.engine_type        = eENGINE_16K_ZH;
            rc = IOT_Asr_RecordFile_Request(asr_client, "./test_file/test.wav", &config, asr_result_cb);
            if (rc > 0) {
                Log_i("record file %s's request_id %d ", "test.wav", rc);
            } else {
                Log_e("record file %s's asr request fail, err: %d", "test.wav", rc);
            }
            HAL_SleepMs(1000);
        }

        if (DEMO_ASR == DEMO_ASR_REALTIEM) {
            RealTimeAsrConf conf    = {0};
            conf.request_timeout_ms = 10000;
            conf.req_type           = eASR_REALTIME;
            conf.engine_type        = eENGINE_16K_ZH;
            conf.res_type           = eRESPONSE_PER_SLICE;
            conf.voice_format       = eVOICE_WAVE;
            conf.seq                = 0;
            conf.end                = 0;
            conf.need_vad           = 1;
            conf.vad_silence_time   = 300;

            char *audio_buff = (char *)HAL_Malloc(PER_SLICE_SIZE);
            memset(audio_buff, 0, PER_SLICE_SIZE);
            if (!audio_buff) {
                Log_e("malloc audio_buff fail");
                goto REAL_TIME_EXIT;
            }

            // fake realtime audio data
            void *fp = HAL_FileOpen("./test_file/test.wav", "r");
            if (NULL == fp) {
                Log_e("can not open file %s!", "./test_file/test.wav");
                goto REAL_TIME_EXIT;
            }
            long file_size = HAL_FileSize(fp);
            while (!HAL_FileEof(fp) && file_size > 0) {
                int data_len = (file_size > PER_SLICE_SIZE) ? PER_SLICE_SIZE : file_size;
                int read_len = HAL_FileRead(audio_buff, 1, data_len, fp);
                if (data_len != read_len) {
                    Log_e("Read file wrong read_len %d(%d)", read_len, data_len);
                    rc = QCLOUD_ERR_FAILURE;
                    goto REAL_TIME_EXIT;
                }
                file_size -= read_len;
                if (file_size == 0) {  // last slice
                    conf.end = 1;
                }

                // ATTENTION: encode audio_buff data (wav/speex/opus/silk/mp3) by yourself before request
                rc = IOT_Asr_Realtime_Request(asr_client, audio_buff, read_len, &conf, asr_result_cb);
                if (rc > 0) {
                    Log_i("realtime request_id %d ", rc);
                    conf.seq++;
                } else {
                    Log_e("realtime request fail, err: %d", rc);
                    conf.seq = 0;
                }

                if (QCLOUD_ERR_MAX_APPENDING_REQUEST == rc) {
                    rc = IOT_Template_Yield(data_template_client, conf.request_timeout_ms);
                } else {
                    rc = IOT_Template_Yield(data_template_client, 200);
                }

                if (rc == QCLOUD_ERR_MQTT_ATTEMPTING_RECONNECT) {
                    HAL_SleepMs(1000);
                    continue;
                } else if (rc != QCLOUD_RET_SUCCESS && rc != QCLOUD_RET_MQTT_RECONNECTED) {
                    Log_e("Exit loop caused of errCode: %d", rc);
                    break;
                }
            }

        REAL_TIME_EXIT:
            if (audio_buff) {
                HAL_Free(audio_buff);
                audio_buff = NULL;
            }

            if (fp) {
                HAL_FileClose(fp);
                fp = NULL;
            }
        }
        /*Demo usagae for ASR <--------- end */
    }

exit:

#ifdef MULTITHREAD_ENABLED
    IOT_Template_Stop_Yield_Thread(data_template_client);
#endif
    HAL_SleepMs(1000);
    rc = IOT_Template_Destroy(data_template_client);
    rc |= IOT_Asr_Destroy(asr_client);

#ifdef LOG_UPLOAD
    IOT_Log_Upload(true);
    IOT_Log_Fini_Uploader();
#endif

    return rc;
}
