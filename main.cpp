#include <windows.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <fstream>
#include <string>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <functional>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shcore.lib")

using namespace Gdiplus;

#define IDI_ICON1 101

// --- Constants ---
const int TOOLBAR_WIDTH = 180;
const int TOOLBAR_HEIGHT = 500;
const int CORNER_RADIUS = 15;

// --- Enums ---
enum class ButtonType { COLOR, THICKNESS, TOOL, ACTION, SYSTEM };
enum class ToolType { FREEHAND, POLYGON, ARROW, CIRCLE, RECTANGLE, SELECTOR, ERASER, UNDO, POINTER };

// --- UI Structures ---
struct Button {
    int id;
    ButtonType type;
    std::wstring label;
    Rect rect;
    Color color;
    float value;
    std::function<void()> onClick;
    bool isHovered = false;
};

// --- Overlay Drawing Data ---
struct Stroke {
    ToolType tool;
    std::vector<PointF> points;
    Color color;
    float width;
};

// --- Global Variables ---
HINSTANCE hInst;
HWND hWndToolbar;
HWND hWndOverlay;
ULONG_PTR gdiplusToken;
std::vector<Button> buttons;
bool isDrawingMode = true;
bool isCompactMode = false;
std::wstring toastMessage;
DWORD toastExpiryTime = 0;

// --- Forward Declarations ---
void SetupUI();
void RefreshToolbar();
void RefreshOverlay();
void SaveScreenshot();
void SetDrawingMode(bool active);
Color currentColor = Color(255, 255, 255, 255);
float currentThickness = 3.0f;
ToolType currentTool = ToolType::FREEHAND;
ToolType g_toolTypes[] = {ToolType::FREEHAND, ToolType::POLYGON, ToolType::ARROW, ToolType::CIRCLE, ToolType::RECTANGLE, ToolType::SELECTOR, ToolType::ERASER, ToolType::UNDO, ToolType::POINTER};

std::vector<Stroke> strokes;
Stroke currentStroke;
bool isLButtonDown = false;
int selectedStrokeIndex = -1;
PointF lastMousePos;
DWORD lastInteractionTime = 0;
BYTE currentOpacity = 255;
bool isLightTheme = false;
bool isGlassy = true;
UINT_PTR idleTimerId = 0;
DWORD lastColorClickTime = 0;
int lastColorClickId = -1;

Color darkPal[6] = {
    Color(255, 0, 0, 0), Color(255, 100, 0, 0), Color(255, 0, 100, 0), 
    Color(255, 0, 0, 100), Color(255, 80, 80, 80), Color(255, 139, 69, 19)
};
Color lightPal[6] = {
    Color(255, 255, 255, 255), Color(255, 255, 50, 50), Color(255, 50, 255, 50), 
    Color(255, 50, 50, 255), Color(255, 200, 200, 200), Color(255, 255, 165, 0)
};

void SaveSettings() {
    std::ofstream out("settings.bin", std::ios::binary);
    if (out) {
        out.write((char*)&isLightTheme, sizeof(isLightTheme));
        out.write((char*)&isGlassy, sizeof(isGlassy));
        out.write((char*)&isCompactMode, sizeof(isCompactMode));
        out.write((char*)&currentThickness, sizeof(currentThickness));
        out.write((char*)darkPal, sizeof(darkPal));
        out.write((char*)lightPal, sizeof(lightPal));
    }
}

void LoadSettings() {
    std::ifstream in("settings.bin", std::ios::binary);
    if (in) {
        in.read((char*)&isLightTheme, sizeof(isLightTheme));
        in.read((char*)&isGlassy, sizeof(isGlassy));
        in.read((char*)&isCompactMode, sizeof(isCompactMode));
        in.read((char*)&currentThickness, sizeof(currentThickness));
        in.read((char*)darkPal, sizeof(darkPal));
        in.read((char*)lightPal, sizeof(lightPal));
    }
}

void PickColor(int index) {
    CHOOSECOLOR cc = {0};
    static COLORREF cust[16] = {0};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hWndToolbar;
    Color& c = isLightTheme ? lightPal[index] : darkPal[index];
    cc.rgbResult = RGB(c.GetR(), c.GetG(), c.GetB());
    cc.lpCustColors = cust;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColor(&cc)) {
        c = Color(255, GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
        currentColor = c;
        SaveSettings();
        SetupUI();
        RefreshToolbar();
    }
}

// --- Helpers ---
float GetDistance(PointF p, PointF a, PointF b) {
    float l2 = pow(b.X - a.X, 2) + pow(b.Y - a.Y, 2);
    if (l2 == 0.0) return sqrt(pow(p.X - a.X, 2) + pow(p.Y - a.Y, 2));
    float t = std::max(0.0f, std::min(1.0f, ((p.X - a.X) * (b.X - a.X) + (p.Y - a.Y) * (b.Y - a.Y)) / l2));
    PointF proj = { a.X + t * (b.X - a.X), a.Y + t * (b.Y - a.Y) };
    return sqrt(pow(p.X - proj.X, 2) + pow(p.Y - proj.Y, 2));
}

bool HitTest(PointF pt, const Stroke& s) {
    float th = s.width + 10.0f;
    if (s.tool == ToolType::FREEHAND || s.tool == ToolType::POLYGON || s.tool == ToolType::ARROW) {
        for (size_t i = 0; i < s.points.size() - 1; i++) if (GetDistance(pt, s.points[i], s.points[i+1]) < th) return true;
    } else if (s.tool == ToolType::CIRCLE || s.tool == ToolType::RECTANGLE) {
        PointF p1 = s.points[0], p2 = s.points.back();
        RectF r(std::min(p1.X, p2.X), std::min(p1.Y, p2.Y), std::abs(p2.X - p1.X), std::abs(p2.Y - p1.Y));
        if (pt.X >= r.X - th && pt.X <= r.X + r.Width + th && pt.Y >= r.Y - th && pt.Y <= r.Y + r.Height + th) return true;
    }
    return false;
}

// --- Forward Declarations ---
LRESULT CALLBACK ToolbarWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK OverlayWndProc(HWND, UINT, WPARAM, LPARAM);
void InitGDIPlus();
void ShutdownGDIPlus();
void PositionToolbar(HWND hwnd);
void CreateOverlay(HINSTANCE hInstance);
void SetupUI();
void DrawRoundedRect(Graphics& g, const Rect& rect, int radius, const Brush* brush, const Pen* pen);
void DrawStroke(Graphics& g, const Stroke& stroke);
void SetDrawingMode(bool active);
void RefreshOverlay();
void RefreshToolbar();
void SaveScreenshot();
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid);

// --- Entry Point ---
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    // Enable DPI awareness for modern displays
    SetProcessDPIAware();
    
    InitGDIPlus();
    LoadSettings();
    hInst = hInstance;

    WNDCLASSEXW wcex = {0};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc = ToolbarWndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = NULL;
    wcex.lpszClassName = L"ScreenPenToolbarV2";
    RegisterClassExW(&wcex);

    WNDCLASSEXW wcexO = {0};
    wcexO.cbSize = sizeof(WNDCLASSEX);
    wcexO.lpfnWndProc = OverlayWndProc;
    wcexO.hInstance = hInstance;
    wcexO.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wcexO.hCursor = LoadCursor(NULL, IDC_CROSS);
    wcexO.hbrBackground = NULL;
    wcexO.lpszClassName = L"ScreenPenOverlay";
    RegisterClassExW(&wcexO);

    SetupUI();

    // Create Overlay first so it can be the owner of the toolbar
    CreateOverlay(hInstance);

    hWndToolbar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"ScreenPenToolbarV2", L"ScreenPen", WS_POPUP, 0, 0, TOOLBAR_WIDTH, 510,
        hWndOverlay, NULL, hInstance, NULL); // hWndOverlay is the owner

    // No more SetLayeredWindowAttributes here, we use UpdateLayeredWindow
    PositionToolbar(hWndToolbar);
    ShowWindow(hWndToolbar, nCmdShow);
    
    lastInteractionTime = GetTickCount();
    idleTimerId = SetTimer(hWndToolbar, 1, 1000, NULL); // Check idle every second

    RefreshToolbar();
    RefreshOverlay();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    ShutdownGDIPlus();
    return (int)msg.wParam;
}

void SetupUI() {
    buttons.clear();
    
    if (isCompactMode) {
        int cw = 50; // Extra slim width
        buttons.push_back({102, ButtonType::SYSTEM, L"=", Rect((cw-25)/2, 5, 25, 25), Color::Transparent, 0, []() { 
            isCompactMode = false; 
            SetupUI(); 
            SetWindowPos(hWndToolbar, NULL, 0, 0, 180, 510, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RefreshToolbar();
        }});

        // Only basic drawing tools
        std::wstring labels[] = {L"~", L"O", L"[]"};
        ToolType tools[] = {ToolType::FREEHAND, ToolType::CIRCLE, ToolType::RECTANGLE};
        for (int i = 0; i < 3; i++) {
            buttons.push_back({400+i, ButtonType::TOOL, labels[i], Rect((cw-32)/2, 40 + i * 40, 32, 32), Color::Transparent, 0, [t = tools[i]]() { 
                currentTool = t; SetDrawingMode(true); 
            }});
        }

        // Pen Tips (2, 3, 4)
        float compactSizes[] = {2, 3, 4};
        for (int i = 0; i < 3; i++) {
            buttons.push_back({250+i, ButtonType::THICKNESS, L"", Rect((cw-20)/2, 160 + i * 25, 20, 20), Color::Transparent, compactSizes[i], [sz = compactSizes[i]]() { 
                currentThickness = sz; RefreshToolbar(); 
            }});
        }

        // Colors (6)
        Color* pal = isLightTheme ? lightPal : darkPal;
        for (int i = 0; i < 6; i++) {
            buttons.push_back({300+i, ButtonType::COLOR, L"", Rect((cw-26)/2, 235 + i * 30, 26, 26), pal[i], 0, [i]() { 
                DWORD now = GetTickCount();
                if (lastColorClickId == 300+i && (now - lastColorClickTime) < 500) {
                    PickColor(i);
                } else {
                    currentColor = isLightTheme ? lightPal[i] : darkPal[i];
                }
                lastColorClickTime = now;
                lastColorClickId = 300+i;
            }});
        }

        SetWindowPos(hWndToolbar, NULL, 0, 0, 50, 430, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        buttons.push_back({100, ButtonType::SYSTEM, L"_", Rect(TOOLBAR_WIDTH - 60, 5, 25, 25), Color::Transparent, 0, []() { ShowWindow(hWndToolbar, SW_MINIMIZE); }});
        buttons.push_back({102, ButtonType::SYSTEM, L"=", Rect(TOOLBAR_WIDTH - 85, 5, 25, 25), Color::Transparent, 0, []() { 
            isCompactMode = true; 
            SetupUI(); 
            SetWindowPos(hWndToolbar, NULL, 0, 0, 50, 430, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RefreshToolbar();
        }});
        buttons.push_back({101, ButtonType::SYSTEM, L"X", Rect(TOOLBAR_WIDTH - 30, 5, 25, 25), Color::Transparent, 0, []() { PostQuitMessage(0); }});

        float sizes[] = {1, 2, 4, 6, 8, 12};
        for (int i = 0; i < 6; i++) {
            buttons.push_back({200 + i, ButtonType::THICKNESS, L"", Rect(15 + i * 26, 60, 20, 20), Color::Transparent, sizes[i], [sz = sizes[i]]() { currentThickness = sz; }});
        }

        Color palette[] = {
            Color(255, 255, 255, 0), Color(255, 255, 128, 0), Color(255, 255, 0, 128), Color(255, 255, 0, 0),
            Color(255, 100, 50, 150), Color(255, 180, 20, 150), Color(255, 220, 10, 100), Color(255, 0, 80, 150),
            Color(255, 0, 150, 255), Color(255, 50, 200, 255), Color(255, 0, 120, 50), Color(255, 120, 200, 10),
            Color(255, 0, 0, 0), Color(255, 50, 50, 50), Color(255, 150, 150, 150), Color(255, 255, 255, 255)
        };
        for (int i = 0; i < 16; i++) {
            buttons.push_back({300 + i, ButtonType::COLOR, L"", Rect(15 + (i % 4) * 40, 100 + (i / 4) * 40, 32, 32), palette[i], 0, [c = palette[i]]() { currentColor = c; }});
        }

        std::wstring labels[] = {L"~", L"Z", L"->", L"O", L"[]", L"Sel", L"E", L"U", L"Mouse"};
        int toolCount = 9;
        for (int i = 0; i < toolCount; i++) {
            buttons.push_back({400 + i, ButtonType::TOOL, labels[i], Rect(15 + (i % 4) * 40, 280 + (i / 4) * 40, 32, 32), Color::Transparent, 0, [t = g_toolTypes[i]]() { 
                currentTool = t;
                if (t == ToolType::UNDO) {
                    if (!strokes.empty()) strokes.pop_back();
                    RefreshOverlay();
                } else if (t == ToolType::POINTER) {
                    SetDrawingMode(false);
                } else {
                    SetDrawingMode(true);
                }
            }});
        }

        // Bottom Utilities in Big Mode
        int by = 420;
        buttons.push_back({500, ButtonType::ACTION, L"CAPTURE", Rect(15, by, 70, 25), Color::Transparent, 0, []() { SaveScreenshot(); }});
        buttons.push_back({501, ButtonType::ACTION, L"CLEAR ALL", Rect(95, by, 70, 25), Color::Transparent, 0, []() { strokes.clear(); RefreshOverlay(); }});
        
        buttons.push_back({350, ButtonType::ACTION, isLightTheme ? L"LIGHT" : L"DARK", Rect(15, by + 30, 70, 25), Color::Transparent, 0, []() { 
            isLightTheme = !isLightTheme; SaveSettings(); SetupUI(); RefreshToolbar(); 
        }});
        buttons.push_back({351, ButtonType::ACTION, isGlassy ? L"GLASS ON" : L"GLASS OFF", Rect(95, by + 30, 70, 25), Color::Transparent, 0, []() { 
            isGlassy = !isGlassy; RefreshToolbar(); 
        }});

        SetWindowPos(hWndToolbar, NULL, 0, 0, 180, 520, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void ShowToast(std::wstring msg) {
    toastMessage = msg;
    toastExpiryTime = GetTickCount() + 3000; // 3 seconds
    RefreshOverlay();
}

void SetDrawingMode(bool active) {
    isDrawingMode = active;
    LONG exStyle = GetWindowLong(hWndOverlay, GWL_EXSTYLE);
    if (active) {
        exStyle &= ~WS_EX_TRANSPARENT;
        ShowWindow(hWndOverlay, SW_SHOW);
    } else {
        exStyle |= WS_EX_TRANSPARENT;
    }
    SetWindowLong(hWndOverlay, GWL_EXSTYLE, exStyle);
    
    SetWindowPos(hWndOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetWindowPos(hWndToolbar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    if (active) {
        SetForegroundWindow(hWndOverlay);
        SetCursor(LoadCursor(NULL, IDC_CROSS));
    } else {
        SetForegroundWindow(hWndToolbar);
    }
    RefreshOverlay();
}

void RefreshOverlay() {
    if (!hWndOverlay) return;
    RECT rc; GetWindowRect(hWndOverlay, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, w, h);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hBmp);

    {
        Graphics g(hMemDC);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetCompositingQuality(CompositingQualityHighQuality);
        
        // Use a very low alpha (1) for the background so it's invisible but catches mouse events
        g.Clear(Color(1, 0, 0, 0)); 
        
        for (const auto& s : strokes) DrawStroke(g, s);
        if (isLButtonDown && currentTool != ToolType::SELECTOR) {
            currentStroke.color = currentColor;
            currentStroke.width = currentThickness;
            currentStroke.tool = currentTool;
            DrawStroke(g, currentStroke);
        }

        if (GetTickCount() < toastExpiryTime) {
            FontFamily ffToast(L"Arial");
            Font fToast(&ffToast, 24, FontStyleBold, UnitPixel);
            StringFormat sfToast;
            sfToast.SetAlignment(StringAlignmentFar);
            sfToast.SetLineAlignment(StringAlignmentCenter);
            SolidBrush bToast(Color(255, 0, 180, 0)); // Green text
            RectF rToast((REAL)w - 400, (REAL)h / 2 - 50, 380, 100);
            g.DrawString(toastMessage.c_str(), -1, &fToast, rToast, &sfToast, &bToast);
            // Refresh again later to clear
            InvalidateRect(hWndOverlay, NULL, FALSE);
        }
    }

    POINT ptSrc = { 0, 0 };
    POINT ptDest = { rc.left, rc.top };
    SIZE size = { w, h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(hWndOverlay, hdcScreen, &ptDest, &size, hMemDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hMemDC, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hdcScreen);
}

void RefreshToolbar() {
    if (!hWndToolbar) return;
    int w = isCompactMode ? 50 : 180;
    int h = isCompactMode ? 430 : 520;
    
    HDC hdcScreen = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, w, h);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hBmp);

    {
        Graphics g(hMemDC);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        
        // Transparent background
        g.Clear(Color(0, 0, 0, 0));
        
        // Draw Toolbar Frame - Glassy Style
        GraphicsPath path;
        int r = 15;
        float bw = 1.0f; // Border width offset
        RectF glassRect(bw, bw, (float)w - bw*2, (float)h - bw*2);
        
        float r2 = (float)r * 2.0f;
        path.AddArc(glassRect.X, glassRect.Y, r2, r2, 180.0f, 90.0f);
        path.AddArc(glassRect.X + glassRect.Width - r2, glassRect.Y, r2, r2, 270.0f, 90.0f);
        path.AddArc(glassRect.X + glassRect.Width - r2, glassRect.Y + glassRect.Height - r2, r2, r2, 0.0f, 90.0f);
        path.AddArc(glassRect.X, glassRect.Y + glassRect.Height - r2, r2, r2, 90.0f, 90.0f);
        path.CloseFigure();

        // Dark or Light Glass Fill
        BYTE alpha = isGlassy ? 160 : 255;
        Color bgCol = isLightTheme ? Color(alpha, 240, 240, 245) : Color(alpha, 25, 30, 35);
        SolidBrush bgBrush(bgCol); 
        g.FillPath(&bgBrush, &path);

        // Glass Border (Rim Light)
        Color rimCol = isLightTheme ? Color(100, 0, 0, 0) : Color(80, 255, 255, 255);
        Pen borderPen(rimCol, 1);
        g.DrawPath(&borderPen, &path);

        FontFamily ff(L"Segoe UI");
        Font f(&ff, 11, FontStyleBold, UnitPixel);
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);

        Color textCol = isLightTheme ? Color(255, 30, 35, 40) : Color(255, 255, 255, 255);
        SolidBrush textBrush(textCol);

        for (auto& btn : buttons) {
            if (btn.type == ButtonType::COLOR) {
                SolidBrush b(btn.color);
                Pen p(Color(255, 255, 255, 255), 2);
                DrawRoundedRect(g, btn.rect, 6, &b, (currentColor.GetValue() == btn.color.GetValue()) ? &p : NULL);
            } else if (btn.type == ButtonType::THICKNESS) {
                SolidBrush b(isLightTheme ? Color(255, 80, 85, 90) : Color(255, 200, 220, 230)); 
                int d = (int)btn.value;
                g.FillEllipse(&b, btn.rect.X + (btn.rect.Width-d)/2, btn.rect.Y + (btn.rect.Height-d)/2, d, d);
                if (currentThickness == btn.value) { 
                    Pen p(Color(255, 0, 150, 255), 2); 
                    g.DrawEllipse(&p, btn.rect.X + (btn.rect.Width-d)/2 - 2, btn.rect.Y + (btn.rect.Height-d)/2 - 2, d+4, d+4); 
                }
            } else if (btn.type == ButtonType::TOOL) {
                bool isActive = false;
                for(int i=0; i<9; i++) if(g_toolTypes[i] == currentTool && btn.id == 400+i) isActive = true;
                
                if (btn.isHovered || (isActive && isDrawingMode)) {
                    SolidBrush b(isActive && isDrawingMode ? Color(180, 0, 120, 215) : Color(60, 255, 255, 255));
                    DrawRoundedRect(g, btn.rect, 8, &b, NULL);
                }
                
                Pen p(textCol, 2); // Opaque icons matching theme
                int cx = btn.rect.X + btn.rect.Width/2, cy = btn.rect.Y + btn.rect.Height/2, s = 8;
                if (btn.label == L"~") { g.DrawBezier(&p, Point(cx-s, cy), Point(cx-s/2, cy-s), Point(cx+s/2, cy+s), Point(cx+s, cy)); }
                else if (btn.label == L"O") { g.DrawEllipse(&p, cx-s, cy-s, s*2, s*2); }
                else if (btn.label == L"[]") { g.DrawRectangle(&p, cx-s, cy-s, s*2, s*2); }
                else { 
                    g.DrawString(btn.label.c_str(), -1, &f, RectF((float)btn.rect.X, (float)btn.rect.Y, (float)btn.rect.Width, (float)btn.rect.Height), &sf, &textBrush); 
                }
            } else if (btn.type == ButtonType::ACTION || btn.type == ButtonType::SYSTEM) {
                if (btn.isHovered) {
                    SolidBrush b(btn.label == L"X" ? Color(200, 230, 50, 50) : (isLightTheme ? Color(100, 0, 0, 0) : Color(80, 255, 255, 255)));
                    DrawRoundedRect(g, btn.rect, btn.type == ButtonType::SYSTEM ? 5 : 10, &b, NULL);
                }
                g.DrawString(btn.label.c_str(), -1, &f, RectF((float)btn.rect.X, (float)btn.rect.Y, (float)btn.rect.Width, (float)btn.rect.Height), &sf, &textBrush);
            }
        }
    }

    POINT ptSrc = { 0, 0 };
    POINT ptDest = { 0, 0 };
    RECT rcW; GetWindowRect(hWndToolbar, &rcW);
    ptDest.x = rcW.left; ptDest.y = rcW.top;
    SIZE size = { w, h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, currentOpacity, AC_SRC_ALPHA };

    UpdateLayeredWindow(hWndToolbar, hdcScreen, &ptDest, &size, hMemDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hMemDC, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hdcScreen);
}

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL) return -1;
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

void SaveScreenshot() {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hdcScreen = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, w, h);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hBmp);
    BitBlt(hMemDC, 0, 0, w, h, hdcScreen, x, y, SRCCOPY);

    {
        Graphics g(hMemDC);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        for (const auto& s : strokes) DrawStroke(g, s);
        
        // Also draw the toolbar manually into the screenshot if needed, 
        // but since we aren't hiding it, BitBlt already captured it!
    }

    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) != -1) {
        Bitmap bmp(hBmp, NULL);
        SYSTEMTIME st; GetLocalTime(&st);
        WCHAR filename[MAX_PATH];
        swprintf_s(filename, L"Capture_%04d%02d%02d_%02d%02d%02d.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        bmp.Save(filename, &pngClsid, NULL);
        ShowToast(L"Screenshot Saved!");
    }

    SelectObject(hMemDC, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hdcScreen);
}

void DrawStroke(Graphics& g, const Stroke& stroke) {
    if (stroke.points.size() < 2) return;
    Pen p(stroke.color, stroke.width);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);

    if (stroke.tool == ToolType::FREEHAND || stroke.tool == ToolType::POLYGON) {
        g.DrawLines(&p, stroke.points.data(), (int)stroke.points.size());
    } else if (stroke.tool == ToolType::ARROW) {
        PointF start = stroke.points[0];
        PointF end = stroke.points.back();
        g.DrawLine(&p, start, end);
        float angle = atan2(end.Y - start.Y, end.X - start.X);
        float headLen = 15.0f + stroke.width;
        PointF p1(end.X - headLen * cos(angle - 0.5f), end.Y - headLen * sin(angle - 0.5f));
        PointF p2(end.X - headLen * cos(angle + 0.5f), end.Y - headLen * sin(angle + 0.5f));
        g.DrawLine(&p, end, p1);
        g.DrawLine(&p, end, p2);
    } else if (stroke.tool == ToolType::CIRCLE) {
        PointF start = stroke.points[0];
        PointF end = stroke.points.back();
        g.DrawEllipse(&p, RectF(std::min(start.X, end.X), std::min(start.Y, end.Y), std::abs(end.X - start.X), std::abs(end.Y - start.Y)));
    } else if (stroke.tool == ToolType::RECTANGLE) {
        PointF start = stroke.points[0];
        PointF end = stroke.points.back();
        g.DrawRectangle(&p, RectF(std::min(start.X, end.X), std::min(start.Y, end.Y), std::abs(end.X - start.X), std::abs(end.Y - start.Y)));
    }
}

LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_SETCURSOR: {
            if (isDrawingMode) {
                if (currentTool == ToolType::SELECTOR) {
                    POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt);
                    bool overStroke = false;
                    for (const auto& s : strokes) if (HitTest(PointF((REAL)pt.x, (REAL)pt.y), s)) { overStroke = true; break; }
                    SetCursor(LoadCursor(NULL, overStroke ? IDC_SIZEALL : IDC_ARROW));
                } else {
                    SetCursor(LoadCursor(NULL, IDC_CROSS));
                }
                return TRUE;
            }
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            RefreshOverlay();
            EndPaint(hWnd, &ps);
        } break;
        case WM_LBUTTONDOWN: {
            if (!isDrawingMode) return DefWindowProc(hWnd, message, wParam, lParam);
            PointF pt((REAL)LOWORD(lParam), (REAL)HIWORD(lParam));
            if (currentTool == ToolType::SELECTOR) {
                selectedStrokeIndex = -1;
                for (int i = (int)strokes.size() - 1; i >= 0; i--) {
                    if (HitTest(pt, strokes[i])) { selectedStrokeIndex = i; break; }
                }
                lastMousePos = pt;
            } else {
                currentStroke.tool = currentTool;
                currentStroke.color = currentColor;
                currentStroke.width = currentThickness;
                currentStroke.points.clear();
                currentStroke.points.push_back(pt);
            }
            isLButtonDown = true;
            SetCapture(hWnd);
        } break;
        case WM_MOUSEMOVE: {
            PointF pt((REAL)LOWORD(lParam), (REAL)HIWORD(lParam));
            if (isLButtonDown) {
                if (currentTool == ToolType::SELECTOR && selectedStrokeIndex != -1) {
                    float dx = pt.X - lastMousePos.X, dy = pt.Y - lastMousePos.Y;
                    for (auto& p : strokes[selectedStrokeIndex].points) { p.X += dx; p.Y += dy; }
                    lastMousePos = pt;
                } else if (currentTool != ToolType::SELECTOR) {
                    if (currentTool == ToolType::FREEHAND) {
                        currentStroke.points.push_back(pt);
                    } else {
                        if (currentStroke.points.size() < 2) currentStroke.points.push_back(pt);
                        else currentStroke.points[1] = pt;
                    }
                }
                RefreshOverlay();
            } else if (currentTool == ToolType::SELECTOR) {
                // Force cursor update
                SendMessage(hWnd, WM_SETCURSOR, (WPARAM)hWnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            }
        } break;
        case WM_LBUTTONUP: {
            if (isLButtonDown) {
                isLButtonDown = false;
                ReleaseCapture();
                if (currentTool != ToolType::SELECTOR && currentStroke.points.size() >= 2) {
                    strokes.push_back(currentStroke);
                }
                selectedStrokeIndex = -1;
                currentStroke.points.clear();
                RefreshOverlay();
            }
        } break;
        case WM_XBUTTONDOWN: {
            int button = GET_XBUTTON_WPARAM(wParam);
            if (button == XBUTTON1) { // Mouse 5 -> Clear
                strokes.clear(); RefreshOverlay();
            } else if (button == XBUTTON2) { // Mouse 6 -> Undo
                if (!strokes.empty()) strokes.pop_back(); RefreshOverlay();
            }
            return TRUE;
        } break;
        case WM_RBUTTONDOWN: SetDrawingMode(false); break;
        case WM_CLOSE: PostQuitMessage(0); break;
        case WM_DESTROY: PostQuitMessage(0); break;
        case WM_KEYDOWN: {
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == 'Z')) {
                if (!strokes.empty()) { strokes.pop_back(); RefreshOverlay(); }
            } else if (wParam == VK_DELETE) {
                strokes.clear(); RefreshOverlay();
            }
        } break;
        case WM_SYSKEYDOWN: {
            if (wParam == VK_F4 && (lParam & (1 << 29))) PostQuitMessage(0);
        } break;
        default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK ToolbarWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: {
            if (wParam == 1) {
                DWORD elapsed = GetTickCount() - lastInteractionTime;
                BYTE targetOpacity = (elapsed > 10000) ? 128 : 255;
                if (targetOpacity != currentOpacity) {
                    currentOpacity = targetOpacity;
                    RefreshToolbar();
                }
            }
        } break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            RefreshToolbar();
            EndPaint(hWnd, &ps);
        } break;
        case WM_NCHITTEST: { 
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            ScreenToClient(hWnd, &pt);
            for (const auto& btn : buttons) {
                if (pt.x >= btn.rect.X && pt.x <= btn.rect.X + btn.rect.Width && 
                    pt.y >= btn.rect.Y && pt.y <= btn.rect.Y + btn.rect.Height) return HTCLIENT;
            }
            return HTCAPTION;
        }
        case WM_MOUSEMOVE: {
            lastInteractionTime = GetTickCount();
            if (currentOpacity != 255) { currentOpacity = 255; RefreshToolbar(); }
            int x = LOWORD(lParam), y = HIWORD(lParam); bool c = false;
            for (auto& btn : buttons) { bool h = (x >= btn.rect.X && x <= btn.rect.X + btn.rect.Width && y >= btn.rect.Y && y <= btn.rect.Y + btn.rect.Height); if (h != btn.isHovered) { btn.isHovered = h; c = true; } }
            if (c) { RefreshToolbar(); TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 }; TrackMouseEvent(&tme); }
        } break;
        case WM_MOUSELEAVE: { for (auto& btn : buttons) btn.isHovered = false; RefreshToolbar(); } break;
        case WM_LBUTTONDOWN: {
            lastInteractionTime = GetTickCount();
            if (currentOpacity != 255) { currentOpacity = 255; RefreshToolbar(); }
            int x = LOWORD(lParam), y = HIWORD(lParam);
            for (auto& btn : buttons) { if (x >= btn.rect.X && x <= btn.rect.X + btn.rect.Width && y >= btn.rect.Y && y <= btn.rect.Y + btn.rect.Height) { btn.onClick(); RefreshToolbar(); break; } }
        } break;
        case WM_XBUTTONDOWN: {
            int button = GET_XBUTTON_WPARAM(wParam);
            if (button == XBUTTON1) { // Mouse 5 -> Clear
                strokes.clear(); RefreshOverlay();
            } else if (button == XBUTTON2) { // Mouse 6 -> Undo
                if (!strokes.empty()) strokes.pop_back(); RefreshOverlay();
            }
            return TRUE;
        } break;
        case WM_MOVE: {
            if (hWndOverlay) RefreshOverlay();
        } break;
        case WM_KEYDOWN: {
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == 'Z')) {
                if (!strokes.empty()) { strokes.pop_back(); RefreshOverlay(); }
            } else if (wParam == VK_DELETE) {
                strokes.clear(); RefreshOverlay();
            }
        } break;
        case WM_CLOSE: PostQuitMessage(0); break;
        case WM_DESTROY: PostQuitMessage(0); break;
        default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void PositionToolbar(HWND hwnd) { int h = GetSystemMetrics(SM_CYSCREEN); SetWindowPos(hwnd, HWND_TOPMOST, 0, (h - TOOLBAR_HEIGHT)/2, TOOLBAR_WIDTH, TOOLBAR_HEIGHT, SWP_SHOWWINDOW); }
void CreateOverlay(HINSTANCE hInstance) { 
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN); 
    hWndOverlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT, L"ScreenPenOverlay", L"Overlay", WS_POPUP, x, y, w, h, NULL, NULL, hInstance, NULL); 
    // No more SetLayeredWindowAttributes here, we use UpdateLayeredWindow
    ShowWindow(hWndOverlay, SW_SHOW);
}
void InitGDIPlus() { GdiplusStartupInput gsi; GdiplusStartup(&gdiplusToken, &gsi, NULL); }
void ShutdownGDIPlus() { GdiplusShutdown(gdiplusToken); }
void DrawRoundedRect(Graphics& g, const Rect& rect, int radius, const Brush* brush, const Pen* pen) { GraphicsPath path; int d = radius * 2; path.AddArc(rect.X, rect.Y, d, d, 180, 90); path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270, 90); path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0, 90); path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90, 90); path.CloseFigure(); if (brush) g.FillPath(brush, &path); if (pen) g.DrawPath(pen, &path); }
