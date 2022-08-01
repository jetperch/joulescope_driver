/*
 * Copyright 2014-2022 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "device_change_notifier.h"
#include "jsdrv_prv/log.h"
#include <Windows.h>
#include <setupapi.h>
#include <dbt.h>


static device_change_notifier_callback callback_ = NULL;
static void * cookie_ = NULL;
static HANDLE thread_ = NULL;
static HWND    window_ = NULL;
static HDEVNOTIFY windowEvent_;
static HANDLE running_event_ = NULL;
static const LPCWSTR szMainWndClass = L"DeviceChangeWindow";
static const UINT_PTR IDT_TIMER = WM_USER + 1;
static const UINT TIMER_DELAY = 100; // in milliseconds
static const DWORD THREAD_JOIN_DELAY = 2000; // in milliseconds
static const DWORD THREAD_START_DELAY = 2000; // in milliseconds


static LRESULT CALLBACK window_callback(HWND hwnd, UINT nMsg, WPARAM wParam, LPARAM lParam) {
    switch (nMsg) {
        case WM_CLOSE: {
            JSDRV_LOGD1("WM_CLOSE");
            DestroyWindow(hwnd);
            return DefWindowProc(hwnd,  nMsg, wParam, lParam);
        }

        case WM_DESTROY: {
            JSDRV_LOGD1("WM_DESTROY");

            // Remove the device notification
            if (NULL != windowEvent_) {
                UnregisterDeviceNotification(windowEvent_);
                windowEvent_ = NULL;
            }

            KillTimer(hwnd, IDT_TIMER);
            PostQuitMessage(0);
            return DefWindowProc(hwnd,  nMsg, wParam, lParam);
        }

        case WM_DEVICECHANGE: {
            // Should start with DEV_BROADCAST_HDR and validate.
            DEV_BROADCAST_DEVICEINTERFACE* hdr;
            hdr = (DEV_BROADCAST_DEVICEINTERFACE*) lParam;

            // Only handle all devices arrived or all devices removed.
            if ((LOWORD(wParam) != DBT_DEVICEARRIVAL) &&
                (LOWORD(wParam) != DBT_DEVICEREMOVECOMPLETE)) {
                return TRUE;
            }
            JSDRV_LOGD1("WM_DEVICECHANGE (filtered)");

            if (!SetTimer(window_, IDT_TIMER, TIMER_DELAY, 0)) {
                // call immediately on error: don't miss the event!
                if (NULL != callback_) {
                    callback_(cookie_);
                }
            }
            return TRUE;
        }

        case WM_TIMER: {
            JSDRV_LOGD1("WM_TIMER");
            KillTimer(hwnd, IDT_TIMER);
            if (NULL != callback_) {
                callback_(cookie_);
            }
            break;
        }
    }

    return DefWindowProc(hwnd,  nMsg, wParam, lParam);
}

static DWORD WINAPI window_create(LPVOID lpParam) {
    lpParam; // unused
    DEV_BROADCAST_DEVICEINTERFACE NotificationFilter;
    MSG event;
    WNDCLASSEXW* wndclass;
    DWORD rc = 0;

    // Register the hidden window class
    wndclass = (WNDCLASSEXW*) malloc(sizeof(WNDCLASSEXW));
    if (NULL == wndclass) {
        JSDRV_LOGE("window_create malloc failed");
        return 1;
    }
    memset (wndclass, 0, sizeof(WNDCLASSEX));
    wndclass->cbSize = sizeof(WNDCLASSEX);
    wndclass->style = CS_HREDRAW | CS_VREDRAW;
    wndclass->lpfnWndProc = window_callback;
    wndclass->hInstance = 0;
    wndclass->hIcon = LoadIcon (NULL, IDI_APPLICATION);
    wndclass->hCursor = LoadCursor (NULL, IDC_ARROW);
    wndclass->hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wndclass->lpszClassName = szMainWndClass;
    wndclass->hIconSm = LoadIcon (NULL, IDI_APPLICATION);
    if (!RegisterClassExW(wndclass)) {
        JSDRV_LOGE("RegisterClassExW failed");
        return 2;
    }

    window_ = CreateWindowW(
        szMainWndClass,             /* Class name */
        szMainWndClass,             /* Caption */
        WS_OVERLAPPEDWINDOW,        /* Style */
        CW_USEDEFAULT,              /* Initial x (use default) */
        CW_USEDEFAULT,              /* Initial y (use default) */
        CW_USEDEFAULT,              /* Initial x size (use default) */
        CW_USEDEFAULT,              /* Initial y size (use default) */
        NULL,                       /* No parent window */
        NULL,                       /* No menu */
        0,                          /* This program instance */
        NULL);                      /* Creation parameters */
    if (window_ == NULL) { // we have a problem!
        JSDRV_LOGE("CreateWindowW failed");
        return 3;
    }

    ShowWindow (window_, SW_HIDE);
    UpdateWindow (window_);

    /* Set up device notification filter to pass all interface changes */
    ZeroMemory(&NotificationFilter, sizeof(NotificationFilter));
    NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    windowEvent_ = RegisterDeviceNotification(window_,
                                              &NotificationFilter,
                                              DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES );

    if (NULL == windowEvent_) {
        JSDRV_LOGE("RegisterDeviceNotification failed");
        rc = 4;
    } else {
        SetEvent(running_event_);
        while (GetMessage(&event, NULL, 0, 0)) { // event loop
            TranslateMessage(&event);
            DispatchMessage(&event);
        }
    }

    // Unregister the hidden window class
    if (!UnregisterClassW(wndclass->lpszClassName, wndclass->hInstance)) {
        JSDRV_LOGW("UnregisterClassW failed");
    } else {
        free(wndclass);
    }

    window_ = NULL;

    return rc;
}

int device_change_notifier_initialize(device_change_notifier_callback callback, void * cookie) {
    if ((NULL != callback_) || (NULL != thread_) || (NULL != window_) || (NULL != running_event_)) {
        JSDRV_LOGE("already initialized");
        return 1;
    }
    if (NULL == callback) {
        JSDRV_LOGE("no callback provided");
        return 2;
    }
    running_event_ = CreateEvent(
        NULL, // default security attributes
        TRUE, // manual reset event
        FALSE, // start unsignaled
        NULL  // no name
    );
    if (!running_event_) {
        JSDRV_LOGE("CreateEvent failed");
        return 4;
    }

    callback_ = callback;
    cookie_ = cookie;
    thread_ = CreateThread(NULL, 0, window_create, NULL, 0, NULL);
    if (NULL == thread_) {
        JSDRV_LOGE("CreateThread failed");
        return 3;
    }
    if (WAIT_OBJECT_0 != WaitForSingleObject(running_event_, THREAD_START_DELAY)) {
        JSDRV_LOGE("Thread did not start");
        return 5;  // could not start thread cleanly
    }

    return 0;
}

int device_change_notifier_finalize() {
    int rc = 0;
    if ((NULL == thread_) || (NULL == window_)) {
        return rc;
    }
    SendMessage(window_, WM_CLOSE, 0, 0);
    if (WAIT_OBJECT_0 != WaitForSingleObject(thread_, THREAD_JOIN_DELAY)) {
        rc = 1; // could not join thread cleanly
    }
    if (!CloseHandle(thread_)) {
        rc = 2; // could not close cleanly
    }
    CloseHandle(running_event_);
    thread_ = NULL;
    callback_ = NULL;
    running_event_ = NULL;
    return rc;
}
