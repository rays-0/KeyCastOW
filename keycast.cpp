// Copyright © 2014 Brook Hong. All Rights Reserved.
//

// k.vim#cmd msbuild /p:platform=win32 /p:Configuration=Release && .\Release\keycastow.exe
// msbuild keycastow.vcxproj /t:Clean
// rc keycastow.rc && cl -DUNICODE -D_UNICODE keycast.cpp keylog.cpp keycastow.res user32.lib shell32.lib gdi32.lib Comdlg32.lib comctl32.lib

#include <windows.h>
#include <windowsx.h>
#include <Commctrl.h>
#include <stdio.h>

#include <gdiplus.h>
using namespace Gdiplus;

#include "resource.h"
#include "timer.h"
CTimer showTimer;

#define MAXCHARS 4096
WCHAR textBuffer[MAXCHARS];
LPCWSTR textBufferEnd = textBuffer + MAXCHARS;

struct KeyLabel{
    RectF rect;
    WCHAR *text;
    DWORD length;
    DWORD time;
    KeyLabel() {
        text = textBuffer;
        length = 0;
    }
};

DWORD keyStrokeDelay;
DWORD lingerTime;
DWORD fadeDuration;
DWORD labelSpacing;
COLORREF textColor;
COLORREF bgColor;
COLORREF borderColor;
int borderSize;
LOGFONT labelFont;
DWORD bgOpacity, textOpacity, borderOpacity;
UINT tcModifiers = MOD_ALT;
UINT tcKey = 0x42;      // 0x42 is 'b'
int cornerSize;

#define MAXLABELS 60
KeyLabel keyLabels[MAXLABELS];
DWORD labelCount = 0;
RECT desktopRect;

#include "keycast.h"
#include "keylog.h"

WCHAR *szWinName = L"KeyCastOW";
HWND hMainWnd;
HWND hDlgSettings;
HINSTANCE hInstance;
Graphics * g;
Font * fontPlus = NULL;

#define IDI_TRAY       100
#define WM_TRAYMSG     101
#define MENU_CONFIG    32
#define MENU_EXIT      33
#define MENU_RESTORE      34
void updateLayeredWindow(HWND hwnd) {
    RECT rt;
    GetWindowRect(hwnd,&rt);
    Rect rc(0, 0, rt.right-rt.left, rt.bottom-rt.top);
    POINT ptSrc = {0, 0};
    POINT ptDst = {rt.left, rt.top};
    SIZE wndSize = {rc.Width, rc.Height};
    BLENDFUNCTION blendFunction;
    blendFunction.AlphaFormat = AC_SRC_ALPHA;
    blendFunction.BlendFlags = 0;
    blendFunction.BlendOp = AC_SRC_OVER;
    blendFunction.SourceConstantAlpha = 255;
    HDC hdcBuf = g->GetHDC();
    HDC hdc = GetDC(hwnd);
    ::UpdateLayeredWindow(hwnd,hdc,&ptDst,&wndSize,hdcBuf,&ptSrc,0,&blendFunction,2);
    ReleaseDC(hwnd, hdc);
    g->ReleaseHDC(hdcBuf);
}
void eraseLabel(int i) {
    RectF &rt = keyLabels[i].rect;
    RectF rc(rt.X-borderSize, rt.Y-borderSize, rt.Width+2*borderSize+1, rt.Height+2*borderSize+1);
    g->SetClip(rc);
    g->Clear(Color::Color(0, 0x7f,0,0x8f));
    g->ResetClip();
}
#define BR(alpha, bgr) (alpha<<24|bgr>>16|(bgr&0x0000ff00)|(bgr&0x000000ff)<<16)
void updateLabel(int i) {
    eraseLabel(i);

    if(keyLabels[i].length > 0) {
        RectF &rc = keyLabels[i].rect;
        PointF origin(rc.X, rc.Y);
        g->MeasureString(keyLabels[i].text, keyLabels[i].length, fontPlus, origin, &rc);
        rc.Width = (rc.Width < cornerSize) ? cornerSize : rc.Width;
        rc.Height = (rc.Height < cornerSize) ? cornerSize : rc.Height;
        double r = 1.0*keyLabels[i].time/fadeDuration;
        r = (r > 1.0) ? 1.0 : r;
        int bgAlpha = (int)(r*bgOpacity), textAlpha = (int)(r*textOpacity), borderAlpha = (int)(r*borderOpacity);
        GraphicsPath path;
        REAL dx = rc.Width - cornerSize, dy = rc.Height - cornerSize;
        path.AddArc(rc.X, rc.Y, (REAL)cornerSize, (REAL)cornerSize, 170, 90);
        path.AddArc(rc.X + dx, rc.Y, (REAL)cornerSize, (REAL)cornerSize, 270, 90);
        path.AddArc(rc.X + dx, rc.Y + dy, (REAL)cornerSize, (REAL)cornerSize, 0, 90);
        path.AddArc(rc.X, rc.Y + dy, (REAL)cornerSize, (REAL)cornerSize, 90, 90);
        path.AddLine(rc.X, rc.Y + dy, rc.X, rc.Y + cornerSize/2);
        Pen penPlus(Color::Color(BR(borderAlpha, borderColor)), borderSize+0.0f);
        SolidBrush brushPlus(Color::Color(BR(bgAlpha, bgColor)));
        g->DrawPath(&penPlus, &path);
        g->FillPath(&brushPlus, &path);

        SolidBrush textBrushPlus(Color(BR(textAlpha, textColor)));
        g->DrawString( keyLabels[i].text,
                keyLabels[i].length,
                fontPlus,
                origin,
                &textBrushPlus);
    }
}

static int newStrokeCount = 0;
static void startFade() {
    if(newStrokeCount > 0) {
        newStrokeCount -= 100;
    }
    DWORD i = 0;
    BOOL dirty = FALSE;
    for(i = 0; i < labelCount; i++) {
        if(keyLabels[i].time > fadeDuration) {
            keyLabels[i].time -= 100;
        } else if(keyLabels[i].time > 0) {
            keyLabels[i].time -= 100;
            updateLabel(i);
            dirty = TRUE;
        }
    }
    if(dirty) {
        updateLayeredWindow(hMainWnd);
    }
}

bool outOfLine(LPCWSTR text) {
    size_t newLen = wcslen(text);
    if(keyLabels[labelCount-1].text+keyLabels[labelCount-1].length+newLen >= textBufferEnd) {
        wcscpy_s(textBuffer, MAXCHARS, keyLabels[labelCount-1].text);
        keyLabels[labelCount-1].text = textBuffer;
    }
    LPWSTR tmp = keyLabels[labelCount-1].text + keyLabels[labelCount-1].length;
    wcscpy_s(tmp, (textBufferEnd-tmp), text);
    RectF box;
    PointF origin(0, 0);
    g->MeasureString(keyLabels[labelCount-1].text, keyLabels[labelCount-1].length, fontPlus, origin, &box);
    RECT r;
    GetWindowRect(hMainWnd,&r);
    return (r.left+box.Width+2*cornerSize+borderSize*2 >= desktopRect.right);
}
void showText(LPCWSTR text, BOOL forceNewStroke = FALSE) {
    SetWindowPos(hMainWnd,HWND_TOPMOST,0,0,0,0,SWP_NOSIZE|SWP_NOMOVE|SWP_NOACTIVATE);
    size_t newLen = wcslen(text);
    if(forceNewStroke || (newStrokeCount <= 0) || outOfLine(text)) {
        DWORD i;
        for (i = 1; i < labelCount; i++) {
            if(keyLabels[i].time > 0) {
                break;
            }
        }
        for (; i < labelCount; i++) {
            eraseLabel(i-1);
            keyLabels[i-1].text = keyLabels[i].text;
            keyLabels[i-1].length = keyLabels[i].length;
            keyLabels[i-1].time = keyLabels[i].time;
            updateLabel(i-1);
            eraseLabel(i);
        }
        keyLabels[labelCount-1].text = keyLabels[labelCount-2].text + keyLabels[labelCount-2].length;
        if(keyLabels[labelCount-1].text+newLen >= textBufferEnd) {
            keyLabels[labelCount-1].text = textBuffer;
        }
        wcscpy_s(keyLabels[labelCount-1].text, textBufferEnd-keyLabels[labelCount-1].text, text);
        keyLabels[labelCount-1].length = newLen;

        keyLabels[labelCount-1].time = lingerTime+fadeDuration;
        updateLabel(labelCount-1);

        newStrokeCount = keyStrokeDelay;
    } else {
        LPWSTR tmp = keyLabels[labelCount-1].text + keyLabels[labelCount-1].length;
        if(tmp+newLen >= textBufferEnd) {
            tmp = textBuffer;
            keyLabels[labelCount-1].text = tmp;
            keyLabels[labelCount-1].length = newLen;
        } else {
            keyLabels[labelCount-1].length += newLen;
        }
        wcscpy_s(tmp, (textBufferEnd-tmp), text);
        keyLabels[labelCount-1].time = lingerTime+fadeDuration;
        updateLabel(labelCount-1);

        newStrokeCount = keyStrokeDelay;
    }
    updateLayeredWindow(hMainWnd);
}

BOOL ColorDialog ( HWND hWnd, COLORREF &clr ) {
    DWORD dwCustClrs[16] = {
        RGB(0,0,0),
        RGB(0,0,255),
        RGB(0,255,0),
        RGB(128,255,255),
        RGB(255,0,0),
        RGB(255,0,255),
        RGB(255,255,0),
        RGB(192,192,192),
        RGB(127,127,127),
        RGB(0,0,128),
        RGB(0,128,0),
        RGB(0,255,255),
        RGB(128,0,0),
        RGB(255,0,128),
        RGB(128,128,64),
        RGB(255,255,255)
    };
    CHOOSECOLOR dlgColor;
    dlgColor.lStructSize = sizeof(CHOOSECOLOR);
    dlgColor.hwndOwner = hWnd;
    dlgColor.hInstance = NULL;
    dlgColor.lpTemplateName = NULL;
    dlgColor.rgbResult =  clr;
    dlgColor.lpCustColors =  dwCustClrs;
    dlgColor.Flags = CC_ANYCOLOR|CC_RGBINIT;
    dlgColor.lCustData = 0;
    dlgColor.lpfnHook = NULL;

    if(ChooseColor(&dlgColor)) {
        clr = dlgColor.rgbResult;
    }
    return TRUE;
}
void updateMainWindow() {
    HDC hdc = GetDC(hMainWnd);
    HFONT hlabelFont = CreateFontIndirect(&labelFont);
    HFONT hFontOld = (HFONT)SelectObject(hdc, hlabelFont);
    DeleteObject(hFontOld);

    if(fontPlus) {
        delete fontPlus;
    }
    fontPlus = new Font(hdc, hlabelFont);
    ReleaseDC(hMainWnd, hdc);
    RectF box;
    PointF origin(0, 0);
    g->MeasureString(L"\u263b - KeyCastOW OFF", 16, fontPlus, origin, &box);
    REAL unitH = box.Height+2*borderSize+labelSpacing;
    labelCount = (desktopRect.bottom - desktopRect.top) / (int)unitH;
    SetWindowPos(hMainWnd, HWND_TOPMOST, desktopRect.right-(int)(box.Width+4*borderSize), 0, desktopRect.right, desktopRect.bottom, 0);

    if(labelCount > MAXLABELS)
        labelCount = MAXLABELS;

    g->Clear(Color::Color(0, 0x7f,0,0x8f));
    for(DWORD i = 0; i < labelCount; i ++) {
        keyLabels[i].time = 0;
        keyLabels[i].rect.X = (REAL)borderSize;
        keyLabels[i].rect.Y = unitH*i;
        if(keyLabels[i].time > lingerTime+fadeDuration) {
            keyLabels[i].time = lingerTime+fadeDuration;
        }
        if(keyLabels[i].time > 0) {
            updateLabel(i);
        }
    }

}
void initSettings() {
    keyStrokeDelay = 500;
    lingerTime = 1200;
    fadeDuration = 600;
    labelSpacing = 30;
    textColor = RGB(255, 255, 255);
    bgColor = RGB(75, 75, 75);
    bgOpacity = textOpacity = borderOpacity = 198;
    borderColor = RGB(0, 128, 255);
    borderSize = 8;
    cornerSize = 16;
    tcModifiers = MOD_ALT;
    tcKey = 0x42;
    memset(&labelFont, 0, sizeof(labelFont));
    labelFont.lfCharSet = DEFAULT_CHARSET;
    labelFont.lfHeight = -37;
    labelFont.lfPitchAndFamily = DEFAULT_PITCH;
    labelFont.lfWeight  = FW_BLACK;
    labelFont.lfOutPrecision = OUT_DEFAULT_PRECIS;
    labelFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    labelFont.lfQuality = ANTIALIASED_QUALITY;
    // incidently clear global variables in debug mode
    wcscpy_s(labelFont.lfFaceName, LF_FACESIZE, TEXT("Arial Black"));
}
BOOL saveSettings() {
    BOOL res = TRUE;

    HKEY hRootKey, hChildKey;
    if(RegOpenCurrentUser(KEY_WRITE, &hRootKey) != ERROR_SUCCESS)
        return FALSE;

    if(RegCreateKeyEx(hRootKey, L"Software\\KeyCastOW", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hChildKey, NULL) != ERROR_SUCCESS) {
        RegCloseKey(hRootKey);
        return FALSE;
    }

    if(RegSetKeyValue(hChildKey, NULL, L"keyStrokeDelay", REG_DWORD, (LPCVOID)&keyStrokeDelay, sizeof(keyStrokeDelay)) != ERROR_SUCCESS) {
        res = FALSE;
    }

    RegSetKeyValue(hChildKey, NULL, L"lingerTime", REG_DWORD, (LPCVOID)&lingerTime, sizeof(lingerTime));
    RegSetKeyValue(hChildKey, NULL, L"fadeDuration", REG_DWORD, (LPCVOID)&fadeDuration, sizeof(fadeDuration));
    RegSetKeyValue(hChildKey, NULL, L"labelSpacing", REG_DWORD, (LPCVOID)&labelSpacing, sizeof(labelSpacing));
    RegSetKeyValue(hChildKey, NULL, L"bgColor", REG_DWORD, (LPCVOID)&bgColor, sizeof(bgColor));
    RegSetKeyValue(hChildKey, NULL, L"textColor", REG_DWORD, (LPCVOID)&textColor, sizeof(textColor));
    RegSetKeyValue(hChildKey, NULL, L"labelFont", REG_BINARY, (LPCVOID)&labelFont, sizeof(labelFont));
    RegSetKeyValue(hChildKey, NULL, L"bgOpacity", REG_DWORD, (LPCVOID)&bgOpacity, sizeof(bgOpacity));
    RegSetKeyValue(hChildKey, NULL, L"textOpacity", REG_DWORD, (LPCVOID)&textOpacity, sizeof(textOpacity));
    RegSetKeyValue(hChildKey, NULL, L"borderOpacity", REG_DWORD, (LPCVOID)&borderOpacity, sizeof(borderOpacity));
    RegSetKeyValue(hChildKey, NULL, L"tcModifiers", REG_DWORD, (LPCVOID)&tcModifiers, sizeof(tcModifiers));
    RegSetKeyValue(hChildKey, NULL, L"tcKey", REG_DWORD, (LPCVOID)&tcKey, sizeof(tcKey));
    RegSetKeyValue(hChildKey, NULL, L"borderColor", REG_DWORD, (LPCVOID)&borderColor, sizeof(borderColor));
    RegSetKeyValue(hChildKey, NULL, L"borderSize", REG_DWORD, (LPCVOID)&borderSize, sizeof(borderSize));
    RegSetKeyValue(hChildKey, NULL, L"cornerSize", REG_DWORD, (LPCVOID)&cornerSize, sizeof(cornerSize));

    RegCloseKey(hRootKey);
    RegCloseKey(hChildKey);
    return res;
}
BOOL loadSettings() {
    BOOL res = TRUE;
    HKEY hRootKey, hChildKey;
    DWORD disposition; // For checking if key was created or only opened
    initSettings();
    if(RegOpenCurrentUser(KEY_WRITE | KEY_READ, &hRootKey) != ERROR_SUCCESS)
        return FALSE;
    if(RegCreateKeyEx(hRootKey, TEXT("SOFTWARE\\KeyCastOW"), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                NULL, &hChildKey, &disposition) != ERROR_SUCCESS) {
        RegCloseKey(hRootKey);
        return FALSE;
    }

    DWORD size = sizeof(DWORD);
    if(disposition == REG_OPENED_EXISTING_KEY) {
        RegGetValue(hChildKey, NULL, L"keyStrokeDelay", RRF_RT_DWORD, NULL, &keyStrokeDelay, &size);
        RegGetValue(hChildKey, NULL, L"lingerTime", RRF_RT_DWORD, NULL, &lingerTime, &size);
        RegGetValue(hChildKey, NULL, L"fadeDuration", RRF_RT_DWORD, NULL, &fadeDuration, &size);
        RegGetValue(hChildKey, NULL, L"labelSpacing", RRF_RT_DWORD, NULL, &labelSpacing, &size);
        RegGetValue(hChildKey, NULL, L"bgColor", RRF_RT_DWORD, NULL, &bgColor, &size);
        RegGetValue(hChildKey, NULL, L"textColor", RRF_RT_DWORD, NULL, &textColor, &size);
        RegGetValue(hChildKey, NULL, L"bgOpacity", RRF_RT_DWORD, NULL, &bgOpacity, &size);
        RegGetValue(hChildKey, NULL, L"textOpacity", RRF_RT_DWORD, NULL, &textOpacity, &size);
        RegGetValue(hChildKey, NULL, L"borderOpacity", RRF_RT_DWORD, NULL, &borderOpacity, &size);
        RegGetValue(hChildKey, NULL, L"tcModifiers", RRF_RT_DWORD, NULL, &tcModifiers, &size);
        RegGetValue(hChildKey, NULL, L"tcKey", RRF_RT_DWORD, NULL, &tcKey, &size);
        RegGetValue(hChildKey, NULL, L"borderColor", RRF_RT_DWORD, NULL, &borderColor, &size);
        RegGetValue(hChildKey, NULL, L"borderSize", RRF_RT_DWORD, NULL, &borderSize, &size);
        RegGetValue(hChildKey, NULL, L"cornerSize", RRF_RT_DWORD, NULL, &cornerSize, &size);

        size = sizeof(labelFont);
        RegGetValue(hChildKey, NULL, L"labelFont", RRF_RT_REG_BINARY, NULL, &labelFont, &size);
    } else {
        saveSettings();
    }

    RegCloseKey(hRootKey);
    RegCloseKey(hChildKey);
    return res;
}
void renderSettingsData(HWND hwndDlg) {
    WCHAR tmp[256];
    swprintf(tmp, 256, L"%d", keyStrokeDelay);
    SetDlgItemText(hwndDlg, IDC_KEYSTROKEDELAY, tmp);
    swprintf(tmp, 256, L"%d", lingerTime);
    SetDlgItemText(hwndDlg, IDC_LINGERTIME, tmp);
    swprintf(tmp, 256, L"%d", fadeDuration);
    SetDlgItemText(hwndDlg, IDC_FADEDURATION, tmp);
    swprintf(tmp, 256, L"%d", labelSpacing);
    SetDlgItemText(hwndDlg, IDC_LABELSPACING, tmp);
    swprintf(tmp, 256, L"%d", bgOpacity);
    SetDlgItemText(hwndDlg, IDC_BGOPACITY, tmp);
    swprintf(tmp, 256, L"%d", textOpacity);
    SetDlgItemText(hwndDlg, IDC_TEXTOPACITY, tmp);
    swprintf(tmp, 256, L"%d", borderOpacity);
    SetDlgItemText(hwndDlg, IDC_BORDEROPACITY, tmp);
    swprintf(tmp, 256, L"%d", borderSize);
    SetDlgItemText(hwndDlg, IDC_BORDERSIZE, tmp);
    swprintf(tmp, 256, L"%d", cornerSize);
    SetDlgItemText(hwndDlg, IDC_CORNERSIZE, tmp);
    CheckDlgButton(hwndDlg, IDC_MODCTRL, (tcModifiers & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_MODALT, (tcModifiers & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_MODSHIFT, (tcModifiers & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_MODWIN, (tcModifiers & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED);
    swprintf(tmp, 256, L"%c", MapVirtualKey(tcKey, MAPVK_VK_TO_CHAR));
    SetDlgItemText(hwndDlg, IDC_TCKEY, tmp);
}
BOOL CALLBACK SettingsWndProc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    WCHAR tmp[256];
    switch (msg)
    {
        case WM_INITDIALOG:
            {
                renderSettingsData(hwndDlg);
                RECT r;
                GetWindowRect(hwndDlg, &r);
                SetWindowPos(hwndDlg, 0, desktopRect.right - r.right + r.left, desktopRect.bottom - r.bottom + r.top, 0, 0, SWP_NOSIZE);
            }
            return TRUE;
        case WM_NOTIFY:
            switch (((LPNMHDR)lParam)->code)
            {

                case NM_CLICK:          // Fall through to the next case.
                case NM_RETURN:
                    {
                        PNMLINK pNMLink = (PNMLINK)lParam;
                        LITEM   item    = pNMLink->item;
                        ShellExecute(NULL, L"open", item.szUrl, NULL, NULL, SW_SHOW);
                        break;
                    }
            }

            break;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_TEXTFONT:
                    {
                        CHOOSEFONT cf ;
                        cf.lStructSize    = sizeof (CHOOSEFONT) ;
                        cf.hwndOwner      = hwndDlg ;
                        cf.hDC            = NULL ;
                        cf.lpLogFont      = &labelFont ;
                        cf.iPointSize     = 0 ;
                        cf.Flags          = CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS | CF_EFFECTS ;
                        cf.rgbColors      = 0 ;
                        cf.lCustData      = 0 ;
                        cf.lpfnHook       = NULL ;
                        cf.lpTemplateName = NULL ;
                        cf.hInstance      = NULL ;
                        cf.lpszStyle      = NULL ;
                        cf.nFontType      = 0 ;               // Returned from ChooseFont
                        cf.nSizeMin       = 0 ;
                        cf.nSizeMax       = 0 ;

                        if(ChooseFont (&cf)) {
                            updateMainWindow();
                            saveSettings();
                        }
                    }
                    return TRUE;
                case IDC_TEXTCOLOR:
                    if( ColorDialog(hwndDlg, textColor) ) {
                        updateMainWindow();
                        saveSettings();
                    }
                    return TRUE;
                case IDC_BGCOLOR:
                    if( ColorDialog(hwndDlg, bgColor) ) {
                        updateMainWindow();
                        saveSettings();
                    }
                    return TRUE;
                case IDC_BORDERCOLOR:
                    if( ColorDialog(hwndDlg, borderColor) ) {
                        updateMainWindow();
                        saveSettings();
                    }
                    return TRUE;
                case IDOK:
                    GetDlgItemText(hwndDlg, IDC_KEYSTROKEDELAY, tmp, 256);
                    keyStrokeDelay = _wtoi(tmp);
                    GetDlgItemText(hwndDlg, IDC_LINGERTIME, tmp, 256);
                    lingerTime = _wtoi(tmp);
                    GetDlgItemText(hwndDlg, IDC_FADEDURATION, tmp, 256);
                    fadeDuration = _wtoi(tmp);
                    GetDlgItemText(hwndDlg, IDC_LABELSPACING, tmp, 256);
                    labelSpacing = _wtoi(tmp);
                    GetDlgItemText(hwndDlg, IDC_BGOPACITY, tmp, 256);
                    bgOpacity = _wtoi(tmp);
                    GetDlgItemText(hwndDlg, IDC_TEXTOPACITY, tmp, 256);
                    textOpacity = _wtoi(tmp);
                    GetDlgItemText(hwndDlg, IDC_BORDEROPACITY, tmp, 256);
                    borderOpacity = _wtoi(tmp);
                    GetDlgItemText(hwndDlg, IDC_BORDERSIZE, tmp, 256);
                    borderSize = _wtoi(tmp);
                    GetDlgItemText(hwndDlg, IDC_CORNERSIZE, tmp, 256);
                    cornerSize = _wtoi(tmp);
                    cornerSize = (cornerSize - borderSize > 0) ? cornerSize : borderSize + 1;
                    tcModifiers = 0;
                    if(BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MODCTRL)) {
                        tcModifiers |= MOD_CONTROL;
                    }
                    if(BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MODALT)) {
                        tcModifiers |= MOD_ALT;
                    }
                    if(BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MODSHIFT)) {
                        tcModifiers |= MOD_SHIFT;
                    }
                    if(BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MODWIN)) {
                        tcModifiers |= MOD_WIN;
                    }
                    GetDlgItemText(hwndDlg, IDC_TCKEY, tmp, 256);
                    if(tcModifiers != 0 && tmp[0] != '\0') {
                        tcKey = VkKeyScanEx(tmp[0], GetKeyboardLayout(0));
                        UnregisterHotKey(NULL, 1);
                        if (!RegisterHotKey( NULL, 1, tcModifiers | MOD_NOREPEAT, tcKey)) {
                            MessageBox(NULL, L"Unable to register hotkey, you probably need go to settings to redefine your hotkey for toggle capturing.", L"Warning", MB_OK|MB_ICONWARNING);
                        }
                    }
                    updateMainWindow();
                    saveSettings();
                case IDCANCEL:
                    EndDialog(hwndDlg, wParam);
                    SetWindowLong(hMainWnd, GWL_EXSTYLE, GetWindowLong(hMainWnd, GWL_EXSTYLE)| WS_EX_TRANSPARENT);
                    return TRUE;
            }
    }
    return FALSE;
}
void stamp(HWND hwnd) {
    RECT rt;
    GetWindowRect(hwnd,&rt);
    HDC hdc = GetDC(hwnd);
    Rect rc(0, 0, rt.right-rt.left, rt.bottom-rt.top);
    HDC memDC = ::CreateCompatibleDC(hdc);
    //SetBkMode (memDC, TRANSPARENT);
    HBITMAP memBitmap = ::CreateCompatibleBitmap(hdc,rc.Width,rc.Height);
    ::SelectObject(memDC,memBitmap);
    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Pen pen(Color::Color(0x7f,0,0x8f), 0);
    SolidBrush brush(Color::Color(0x7f,0,0x8f));
    GraphicsPath path;
    int d = 60;
    path.AddArc(rc.X, rc.Y, d, d, 180, 90);
    path.AddArc(rc.X + rc.Width - d, rc.Y, d, d, 270, 90);
    path.AddArc(rc.X + rc.Width - d, rc.Y + rc.Height - d, d, d, 0, 90);
    path.AddArc(rc.X, rc.Y + rc.Height - d, d, d, 90, 90);
    path.AddLine(rc.X, rc.Y + rc.Height - d, rc.X, rc.Y + d/2);
    g.DrawPath(&pen, &path);
    g.FillPath(&brush, &path);

    POINT ptSrc = {0, 0};
    SIZE wndSize = {rc.Width, rc.Height};
    BLENDFUNCTION blendFunction;
    blendFunction.AlphaFormat = AC_SRC_ALPHA;
    blendFunction.BlendFlags = 0;
    blendFunction.BlendOp = AC_SRC_OVER;
    blendFunction.SourceConstantAlpha = 180;
    ::UpdateLayeredWindow(hwnd,hdc,&ptSrc,&wndSize,memDC,&ptSrc,0,&blendFunction,2);
    ::DeleteDC(memDC);
    ::DeleteObject(memBitmap);
    ReleaseDC(hwnd, hdc);
}
LRESULT CALLBACK DraggableWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static POINT s_last_mouse;
    switch(message)
    {
        // hold mouse to move
        case WM_LBUTTONDOWN:
            SetCapture(hWnd);
            GetCursorPos(&s_last_mouse);
            showTimer.Stop();
            break;
        case WM_MOUSEMOVE:
            if (GetCapture()==hWnd)
            {
                POINT p;
                GetCursorPos(&p);
                int dx= p.x - s_last_mouse.x;
                int dy= p.y - s_last_mouse.y;
                if (dx||dy)
                {
                    s_last_mouse=p;
                    RECT r;
                    GetWindowRect(hWnd,&r);
                    SetWindowPos(hWnd,HWND_TOPMOST,r.left+dx,r.top+dy,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
                }
            }
            break;
        case WM_LBUTTONUP:
            ReleaseCapture();
            showTimer.Start(100);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}
LRESULT CALLBACK WindowFunc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static POINT s_last_mouse;
    static HMENU hPopMenu;
    static NOTIFYICONDATA nid;

    switch(message) {
        // trayicon
        case WM_CREATE:
            {
                memset( &nid, 0, sizeof( nid ) );

                nid.cbSize              = sizeof( nid );
                nid.hWnd                = hWnd;
                nid.uID                 = IDI_TRAY;
                nid.uFlags              = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                nid.uCallbackMessage    = WM_TRAYMSG;
                nid.hIcon = LoadIcon( hInstance, MAKEINTRESOURCE(IDI_ICON1));
                lstrcpy( nid.szTip, L"KeyCast On Windows by brook hong" );
                Shell_NotifyIcon( NIM_ADD, &nid );

                hPopMenu = CreatePopupMenu();
                AppendMenu( hPopMenu, MF_STRING, MENU_CONFIG,  L"&Settings..." );
                AppendMenu( hPopMenu, MF_STRING, MENU_RESTORE,  L"&Restore default settings" );
                AppendMenu( hPopMenu, MF_STRING, MENU_EXIT,    L"E&xit" );
                SetMenuDefaultItem( hPopMenu, MENU_CONFIG, FALSE );
            }
            break;
        case WM_TRAYMSG:
            {
                switch ( lParam )
                {
                    case WM_RBUTTONUP:
                        {
                            POINT pnt;
                            GetCursorPos( &pnt );
                            SetForegroundWindow( hWnd ); // needed to get keyboard focus
                            TrackPopupMenu( hPopMenu, TPM_LEFTALIGN, pnt.x, pnt.y, 0, hWnd, NULL );
                        }
                        break;
                    case WM_LBUTTONDBLCLK:
                        SendMessage( hWnd, WM_COMMAND, MENU_CONFIG, 0 );
                        return 0;
                }
            }
            break;
        case WM_COMMAND:
            {
                switch ( LOWORD( wParam ) )
                {
                    case MENU_CONFIG:
                        renderSettingsData(hDlgSettings);
                        ShowWindow(hDlgSettings, SW_SHOW);
                        SetWindowLong(hMainWnd, GWL_EXSTYLE, GetWindowLong(hMainWnd, GWL_EXSTYLE)& ~WS_EX_TRANSPARENT);
                        break;
                    case MENU_RESTORE:
                        initSettings();
                        saveSettings();
                        updateMainWindow();
                        break;
                    case MENU_EXIT:
                        Shell_NotifyIcon( NIM_DELETE, &nid );
                        ExitProcess(0);
                        break;
                    default:
                        break;
                }
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DraggableWndProc(hWnd, message, wParam, lParam);
    }
    return 0;
}
ATOM MyRegisterClassEx(HINSTANCE hInst, LPCWSTR className, WNDPROC wndProc) {
    WNDCLASSEX wcl;
    wcl.cbSize = sizeof(WNDCLASSEX);
    wcl.hInstance = hInst;
    wcl.lpszClassName = className;
    wcl.lpfnWndProc = wndProc;
    wcl.style = CS_DBLCLKS;
    wcl.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcl.hIconSm = LoadIcon(NULL, IDI_WINLOGO);
    wcl.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcl.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wcl.lpszMenuName = NULL;
    wcl.cbWndExtra = 0;
    wcl.cbClsExtra = 0;

    return RegisterClassEx(&wcl);
}
int WINAPI WinMain(HINSTANCE hThisInst, HINSTANCE hPrevInst,
        LPSTR lpszArgs, int nWinMode)
{
    MSG        msg;

    hInstance = hThisInst;

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LINK_CLASS|ICC_LISTVIEW_CLASSES|ICC_PAGESCROLLER_CLASS
        |ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES|ICC_TAB_CLASSES|ICC_TREEVIEW_CLASSES
        |ICC_UPDOWN_CLASS|ICC_USEREX_CLASSES|ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR           gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    if(!MyRegisterClassEx(hThisInst, szWinName, WindowFunc)) {
        MessageBox(NULL, L"Could not register window class", L"Error", MB_OK);
        return 0;
    }

    hMainWnd = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            szWinName,
            szWinName,
            WS_POPUP,
            0, 0,            //X and Y position of window
            0, 0,            //Width and height of window
            NULL,
            NULL,
            hThisInst,
            NULL
            );
    if( !hMainWnd)    {
        MessageBox(NULL, L"Could not create window", L"Error", MB_OK);
        return 0;
    }

    //MyRegisterClassEx(hThisInst, L"STAMP", DraggableWndProc);
    //HWND hWndStamp = CreateWindowEx(
            //WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            //L"STAMP", L"STAMP", WS_VISIBLE|WS_POPUP,
            //10, 20, 280, 160,
            //NULL, NULL, hThisInst, NULL);
    //SetWindowPos(hWndStamp,HWND_TOPMOST,0,0,0,0,SWP_NOSIZE|SWP_NOMOVE|SWP_NOACTIVATE);
    //stamp(hWndStamp);

    loadSettings();
    if (!RegisterHotKey( NULL, 1, tcModifiers | MOD_NOREPEAT, tcKey)) {
        MessageBox(NULL, L"Unable to register hotkey, you probably need go to settings to redefine your hotkey for toggle capturing.", L"Warning", MB_OK|MB_ICONWARNING);
    }

    SystemParametersInfo(SPI_GETWORKAREA,NULL,&desktopRect,NULL);
    UpdateWindow(hMainWnd);

    HDC hdc = GetDC(hMainWnd);
    HDC hdcBuffer = CreateCompatibleDC(hdc);
    HBITMAP hbitmap = CreateCompatibleBitmap(hdc, desktopRect.right, desktopRect.bottom);
    HBITMAP hBitmapOld = (HBITMAP)SelectObject(hdcBuffer, (HGDIOBJ)hbitmap);
    ReleaseDC(hMainWnd, hdc);
    DeleteObject(hBitmapOld);
    g = new Graphics(hdcBuffer);
    g->SetSmoothingMode(SmoothingModeAntiAlias);
    g->SetTextRenderingHint(TextRenderingHintAntiAlias);

    updateMainWindow();
    ShowWindow(hMainWnd, SW_SHOW);
    hDlgSettings = CreateDialog(hThisInst, MAKEINTRESOURCE(IDD_DLGSETTINGS), hMainWnd, (DLGPROC)SettingsWndProc);
    HFONT hlabelFont = CreateFont(20,10,0,0,FW_BLACK,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_OUTLINE_PRECIS,
                CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY, VARIABLE_PITCH,TEXT("Arial"));
    HWND hlink = GetDlgItem(hDlgSettings, IDC_SYSLINK1);
    SendMessage(hlink, WM_SETFONT, (WPARAM)hlabelFont, TRUE);

    showTimer.OnTimedEvent = startFade;
    showTimer.Start(100);

    kbdhook = SetWindowsHookEx(WH_KEYBOARD_LL, LLKeyboardProc, hThisInst, NULL);

    while( GetMessage(&msg, NULL, 0, 0) )    {
        if (msg.message == WM_HOTKEY) {
            if(kbdhook) {
                showText(L"\u263b - KeyCastOW OFF", TRUE);
                UnhookWindowsHookEx(kbdhook);
                kbdhook = NULL;
            } else {
                showText(L"\u263b - KeyCastOW ON", TRUE);
                kbdhook = SetWindowsHookEx(WH_KEYBOARD_LL, LLKeyboardProc, hInstance, NULL);
            }
        } else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    UnhookWindowsHookEx(kbdhook);
    UnregisterHotKey(NULL, 1);
    delete g;
    delete fontPlus;

    GdiplusShutdown(gdiplusToken);
    return msg.wParam;
}
