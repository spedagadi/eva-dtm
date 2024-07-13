//
// Created by satee on 13/07/2024.
//

#include "avermedia_capture.h"

#include <stdio.h>

HANDLE g_hObject = NULL;
DWORD g_dwVideoWidth = 1920;
DWORD g_dwVideoHeight = 1080;
DWORD g_dwVideoFrameRate = 3000;

BOOL WINAPI CallbackRaw(AVER_MEDIA_TYPE SampleInfo, BYTE *pbData, LONG lLength,
                        __int64 tRefTime, LONGPTR lpUserData) {
  return TRUE;
}

bool start_avermedia_capture() {
  AVerInitialize();
  DWORD dwDeviceNum = 0;
  int hr = AVerGetDeviceNum(&dwDeviceNum);
  if (hr != CAP_EC_SUCCESS || dwDeviceNum < 1) {
    fprintf(stderr, "\nNo Device! \n");
    getchar();
    return -1;
  }
  hr = AVerCreateCaptureObject(0, NULL, &g_hObject);
  if (hr != CAP_EC_SUCCESS) {
    AVerDeleteCaptureObject(g_hObject);
    printf("\nDevice Failure!\n");
    getchar();
    return -1;
  }
  DWORD dwSourceNum = 0;
  AVerGetVideoSourceSupported(g_hObject, NULL, &dwSourceNum);
  DWORD *pdwSource = (DWORD *)malloc(dwSourceNum);
  AVerGetVideoSourceSupported(g_hObject, pdwSource, &dwSourceNum);
  for (int i = 0; i < dwSourceNum; i++) {
    hr = AVerSetVideoSource(g_hObject, pdwSource[i]);
    BOOL bSignalPresence = FALSE;
    AVerGetSignalPresence(g_hObject, &bSignalPresence);
    if (bSignalPresence) {
      INPUT_VIDEO_INFO InputVideoInfo;
      ZeroMemory(&InputVideoInfo, sizeof(InputVideoInfo));
      InputVideoInfo.dwVersion = 2;
      AVerGetVideoInfo(g_hObject, &InputVideoInfo);
      g_dwVideoWidth = InputVideoInfo.dwWidth;
      g_dwVideoHeight = InputVideoInfo.dwHeight;
      g_dwVideoFrameRate = InputVideoInfo.dwFramerate;
      break;
    } else {
      continue;
    }
  }
  DWORD dwMode;
  AVerGetMacroVisionMode(g_hObject, &dwMode);
  if (dwMode > 0) {
    printf("\nHDCP! \n");
    getchar();
    AVerDeleteCaptureObject(g_hObject);
    return -1;
  }

  /*VIDEO_RESOLUTION sResolution = {0};
  sResolution.dwVersion = 1;
  sResolution.bCustom = TRUE;
  sResolution.dwWidth = g_dwVideoWidth;
  sResolution.dwHeight = g_dwVideoHeight;
  hr = AVerSetVideoResolutionEx(g_hObject, &sResolution);
  if (hr != CAP_EC_SUCCESS) {
    AVerDeleteCaptureObject(g_hObject);
    return -1;
  }
  hr = AVerSetVideoInputFrameRate(g_hObject, g_dwVideoFrameRate);*/
  //*********SetVideoColorSpace**********************
  // 	VIDEOCOLORSPACE_YV12
  // 	VIDEOCOLORSPACE_YUY2
  // 	VIDEOCOLORSPACE_V210
  // 	VIDEOCOLORSPACE_RGB24
  // 	VIDEOCOLORSPACE_RGB32
  hr = AVerSetVideoColorSpace(g_hObject, VIDEOCOLORSPACE_V210);
  if (hr != CAP_EC_SUCCESS) {
    printf("\nThis device not support output color space v210 with resolution "
           "%dx%d ! \n",
           g_dwVideoWidth, g_dwVideoHeight);
    getchar();
    AVerDeleteCaptureObject(g_hObject);
    return -1;
  }
  //*************************************************
  hr = AVerStartStreaming(g_hObject);
  if (hr != CAP_EC_SUCCESS) {
    AVerDeleteCaptureObject(g_hObject);
    return -1;
  }
  CALLBACK_VIDEO_INFO VideoCaptureInfo = {0};
  VideoCaptureInfo.dwVersion = 1;
  VideoCaptureInfo.lpCallback = CallbackRaw;
  VideoCaptureInfo.lCallbackUserData = 0;
  hr = AVerCallbackRawVideoDataStart(g_hObject, &VideoCaptureInfo);
  if (hr < CAP_EC_SUCCESS) {
    AVerDeleteCaptureObject(g_hObject);
    return -1;
  }
  Sleep(2000);
  AVerCallbackRawVideoDataStop(g_hObject);
  return true;
}

void stop_avermedia_capture() {
  AVerStopStreaming(g_hObject);
  AVerDeleteCaptureObject(g_hObject);
  AVerUninitialize();
}