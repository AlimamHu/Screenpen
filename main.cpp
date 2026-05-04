#include <windows.h>
#include <dwmapi.h>
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

// --- Constants ---
const int TOOLBAR_WIDTH = 180;
const int TOOLBAR_HEIGHT = 500;
const int CORNER_RADIUS = 15;

// --- Enums ---
enum class ButtonType { COLOR, THICKNESS, TOOL, ACTION, SYSTEM };
enum class ToolType { FREEHAND, POLYGON, ARROW, CIRCLE, SELECTOR, ERASER, UNDO, POINTER };

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
bool isDrawingMode = false;
Color currentColor = Color(255, 255, 255, 255);
float currentThickness = 3.0f;
ToolType currentTool = ToolType::FREEHAND;
ToolType g_toolTypes[] = {ToolType::FREEHAND, ToolType::POLYGON, ToolType::ARROW, ToolType::CIRCLE, ToolType::SELECTOR, ToolType::ERASER, ToolType::UNDO, ToolType::POINTER};

std::vector<Stroke> strokes;
Stroke currentStroke;
bool isLButtonDown = false;

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

// --- Entry Point ---
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    // Enable DPI awareness for modern displays
    SetProcessDPIAware();
    
    InitGDIPlus();
    hInst = hInstance;

    WNDCLASSEXW wcex = {0};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc = ToolbarWndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = CreateSolidBrush(RGB(180, 180, 190));
    wcex.lpszClassName = L"ScreenPenToolbarV2";
    RegisterClassExW(&wcex);

    WNDCLASSEXW wcexO = {0};
    wcexO.cbSize = sizeof(WNDCLASSEX);
    wcexO.lpfnWndProc = OverlayWndProc;
    wcexO.hInstance = hInstance;
    wcexO.hCursor = LoadCursor(NULL, IDC_CROSS);
    wcexO.hbrBackground = CreateSolidBrush(RGB(255, 0, 255)); // Magenta Key
    wcexO.lpszClassName = L"ScreenPenOverlay";
    RegisterClassExW(&wcexO);

    SetupUI();

    // Create Overlay first so it can be the owner of the toolbar
    CreateOverlay(hInstance);

    hWndToolbar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"ScreenPenToolbarV2", L"ScreenPen", WS_POPUP, 0, 0, TOOLBAR_WIDTH, TOOLBAR_HEIGHT,
        hWndOverlay, NULL, hInstance, NULL); // hWndOverlay is the owner

    SetLayeredWindowAttributes(hWndToolbar, 0, 255, LWA_ALPHA);
    PositionToolbar(hWndToolbar);
    ShowWindow(hWndToolbar, nCmdShow);

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
    buttons.push_back({100, ButtonType::SYSTEM, L"_", Rect(TOOLBAR_WIDTH - 60, 5, 25, 25), Color::Transparent, 0, []() { ShowWindow(hWndToolbar, SW_MINIMIZE); }});
    buttons.push_back({101, ButtonType::SYSTEM, L"X", Rect(TOOLBAR_WIDTH - 30, 5, 25, 25), Color::Transparent, 0, []() { PostQuitMessage(0); }});

    float sizes[] = {2, 5, 10, 15, 20, 30};
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

    std::wstring labels[] = {L"~", L"Z", L"->", L"O", L"Sel", L"E", L"U", L"Ptr"};
    for (int i = 0; i < 8; i++) {
        buttons.push_back({400 + i, ButtonType::TOOL, labels[i], Rect(15 + (i % 4) * 40, 280 + (i / 4) * 40, 32, 32), Color::Transparent, 0, [t = g_toolTypes[i]]() { 
            currentTool = t;
            if (t == ToolType::UNDO) {
                if (!strokes.empty()) strokes.pop_back();
                InvalidateRect(hWndOverlay, NULL, TRUE);
            } else if (t == ToolType::POINTER) {
                SetDrawingMode(false);
            } else {
                SetDrawingMode(true);
            }
        }});
    }

    buttons.push_back({500, ButtonType::ACTION, L"Guardar", Rect(15, 380, TOOLBAR_WIDTH - 30, 40), Color::Transparent, 0, []() { /* Save */ }});
    buttons.push_back({501, ButtonType::ACTION, L"Borrar", Rect(15, 435, TOOLBAR_WIDTH - 30, 40), Color::Transparent, 0, []() { strokes.clear(); InvalidateRect(hWndOverlay, NULL, TRUE); }});
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
    
    // Maintain Z-order: Toolbar on top of Overlay, both Topmost
    // Note: Since hWndToolbar is owned by hWndOverlay, it will naturally stay on top,
    // but we re-assert topmost status just in case.
    SetWindowPos(hWndOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetWindowPos(hWndToolbar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    if (active) {
        SetForegroundWindow(hWndOverlay);
        SetCursor(LoadCursor(NULL, IDC_CROSS));
    } else {
        SetForegroundWindow(hWndToolbar);
    }
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
    }
}

LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_SETCURSOR: {
            if (isDrawingMode) {
                SetCursor(LoadCursor(NULL, IDC_CROSS));
                return TRUE;
            }
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        case WM_KEYDOWN: {
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == 'Z')) {
                if (!strokes.empty()) {
                    strokes.pop_back();
                    InvalidateRect(hWnd, NULL, TRUE);
                }
            } else if (wParam == VK_DELETE) {
                strokes.clear();
                InvalidateRect(hWnd, NULL, TRUE);
            }
        } break;
        case WM_SYSKEYDOWN: {
            // Check for Alt + F4 (SC_CLOSE is handled in WM_SYSCOMMAND usually, but we can catch it here)
            if (wParam == VK_F4 && (lParam & (1 << 29))) { // Alt is bit 29
                PostQuitMessage(0);
            }
        } break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            Graphics g(hdc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            for (const auto& s : strokes) DrawStroke(g, s);
            if (isLButtonDown) {
                currentStroke.color = currentColor;
                currentStroke.width = currentThickness;
                currentStroke.tool = currentTool;
                DrawStroke(g, currentStroke);
            }
            EndPaint(hWnd, &ps);
        } break;
        case WM_LBUTTONDOWN: {
            if (!isDrawingMode) return DefWindowProc(hWnd, message, wParam, lParam);
            isLButtonDown = true;
            currentStroke.tool = currentTool;
            currentStroke.color = currentColor;
            currentStroke.width = currentThickness;
            currentStroke.points.clear();
            currentStroke.points.push_back(PointF((REAL)LOWORD(lParam), (REAL)HIWORD(lParam)));
            SetCapture(hWnd);
        } break;
        case WM_MOUSEMOVE: {
            if (isLButtonDown) {
                PointF pt((REAL)LOWORD(lParam), (REAL)HIWORD(lParam));
                if (currentTool == ToolType::FREEHAND) {
                    currentStroke.points.push_back(pt);
                } else {
                    if (currentStroke.points.size() < 2) currentStroke.points.push_back(pt);
                    else currentStroke.points[1] = pt;
                }
                InvalidateRect(hWnd, NULL, FALSE);
            }
        } break;
        case WM_LBUTTONUP: {
            if (isLButtonDown) {
                isLButtonDown = false;
                ReleaseCapture();
                if (currentStroke.points.size() >= 2) {
                    strokes.push_back(currentStroke);
                }
                currentStroke.points.clear();
                InvalidateRect(hWnd, NULL, TRUE);
            }
        } break;
        case WM_RBUTTONDOWN: SetDrawingMode(false); break;
        case WM_CLOSE: PostQuitMessage(0); break;
        case WM_DESTROY: PostQuitMessage(0); break;
        default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK ToolbarWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            Graphics g(hdc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            SolidBrush bgBrush(Color(255, 180, 180, 190));
            DrawRoundedRect(g, Rect(0, 0, TOOLBAR_WIDTH, TOOLBAR_HEIGHT), CORNER_RADIUS, &bgBrush, NULL);
            FontFamily ff(L"Segoe UI");
            Font f(&ff, 14, FontStyleRegular, UnitPixel);
            StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
            for (auto& btn : buttons) {
                if (btn.type == ButtonType::COLOR) {
                    SolidBrush b(btn.color);
                    Pen p(Color(255, 255, 255, 255), 2);
                    DrawRoundedRect(g, btn.rect, 8, &b, (currentColor.GetValue() == btn.color.GetValue()) ? &p : NULL);
                } else if (btn.type == ButtonType::THICKNESS) {
                    SolidBrush b(Color(255, 60, 80, 80));
                    int d = (int)btn.value;
                    g.FillEllipse(&b, btn.rect.X + (20-d)/2, btn.rect.Y + (20-d)/2, d, d);
                    if (currentThickness == btn.value) { Pen p(Color(255, 255, 255, 255), 2); g.DrawEllipse(&p, btn.rect.X + (20-d)/2 - 2, btn.rect.Y + (20-d)/2 - 2, d+4, d+4); }
                } else if (btn.type == ButtonType::TOOL) {
                    bool isActive = (currentTool == g_toolTypes[btn.id - 400]);
                    Pen p(Color(255, 60, 80, 80), 2);
                    if (isActive && isDrawingMode) p.SetColor(Color(255, 0, 120, 215));
                    int cx = btn.rect.X + 16, cy = btn.rect.Y + 16, s = 8;
                    if (btn.label == L"~") { g.DrawBezier(&p, Point(cx-s, cy), Point(cx-s/2, cy-s), Point(cx+s/2, cy+s), Point(cx+s, cy)); }
                    else if (btn.label == L"Z") { Point pts[] = { Point(cx-s, cy+s), Point(cx, cy-s), Point(cx+s, cy+s) }; g.DrawLines(&p, pts, 3); }
                    else if (btn.label == L"->") { g.DrawLine(&p, cx-s, cy+s, cx+s, cy-s); g.DrawLine(&p, cx+s, cy-s, cx+s-5, cy-s); g.DrawLine(&p, cx+s, cy-s, cx+s, cy-s+5); }
                    else if (btn.label == L"O") { g.DrawEllipse(&p, cx-s, cy-s, s*2, s*2); }
                    else { SolidBrush t(Color(255, 60, 80, 80)); RectF tr((REAL)btn.rect.X, (REAL)btn.rect.Y, 32, 32); g.DrawString(btn.label.c_str(), -1, &f, tr, &sf, &t); }
                } else if (btn.type == ButtonType::ACTION) {
                    SolidBrush b(btn.isHovered ? Color(255, 210, 210, 220) : Color(255, 230, 230, 230)); Pen p(Color(150, 100, 100, 100), 1);
                    DrawRoundedRect(g, btn.rect, 10, &b, &p); SolidBrush t(Color(255, 30, 30, 30));
                    RectF tr((REAL)btn.rect.X, (REAL)btn.rect.Y, (REAL)btn.rect.Width, (REAL)btn.rect.Height); g.DrawString(btn.label.c_str(), -1, &f, tr, &sf, &t);
                } else {
                    SolidBrush t(Color(255, 60, 80, 80)); RectF tr((REAL)btn.rect.X, (REAL)btn.rect.Y, (REAL)btn.rect.Width, (REAL)btn.rect.Height); g.DrawString(btn.label.c_str(), -1, &f, tr, &sf, &t);
                }
            }
            EndPaint(hWnd, &ps);
        } break;
        case WM_NCHITTEST: { 
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            ScreenToClient(hWnd, &pt);
            for (const auto& btn : buttons) {
                if (pt.x >= btn.rect.X && pt.x <= btn.rect.X + btn.rect.Width && 
                    pt.y >= btn.rect.Y && pt.y <= btn.rect.Y + btn.rect.Height) {
                    return HTCLIENT; // Area is a button, handle normally
                }
            }
            return HTCAPTION; // Rest of window is draggable
        }
        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam), y = HIWORD(lParam); bool c = false;
            for (auto& btn : buttons) { bool h = (x >= btn.rect.X && x <= btn.rect.X + btn.rect.Width && y >= btn.rect.Y && y <= btn.rect.Y + btn.rect.Height); if (h != btn.isHovered) { btn.isHovered = h; c = true; } }
            if (c) { InvalidateRect(hWnd, NULL, FALSE); TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 }; TrackMouseEvent(&tme); }
        } break;
        case WM_MOUSELEAVE: { for (auto& btn : buttons) btn.isHovered = false; InvalidateRect(hWnd, NULL, FALSE); } break;
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam), y = HIWORD(lParam);
            for (auto& btn : buttons) { if (x >= btn.rect.X && x <= btn.rect.X + btn.rect.Width && y >= btn.rect.Y && y <= btn.rect.Y + btn.rect.Height) { btn.onClick(); InvalidateRect(hWnd, NULL, FALSE); break; } }
        } break;
        case WM_KEYDOWN: {
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == 'Z')) {
                if (!strokes.empty()) {
                    strokes.pop_back();
                    InvalidateRect(hWndOverlay, NULL, TRUE);
                }
            } else if (wParam == VK_DELETE) {
                strokes.clear();
                InvalidateRect(hWndOverlay, NULL, TRUE);
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
    SetLayeredWindowAttributes(hWndOverlay, RGB(255, 0, 255), 255, LWA_COLORKEY); 
    ShowWindow(hWndOverlay, SW_SHOW);
}
void InitGDIPlus() { GdiplusStartupInput gsi; GdiplusStartup(&gdiplusToken, &gsi, NULL); }
void ShutdownGDIPlus() { GdiplusShutdown(gdiplusToken); }
void DrawRoundedRect(Graphics& g, const Rect& rect, int radius, const Brush* brush, const Pen* pen) { GraphicsPath path; int d = radius * 2; path.AddArc(rect.X, rect.Y, d, d, 180, 90); path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270, 90); path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0, 90); path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90, 90); path.CloseFigure(); if (brush) g.FillPath(brush, &path); if (pen) g.DrawPath(pen, &path); }
