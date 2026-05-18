// MainFrm.cpp: Main Frame Implementation
// Toolbar (AutoCAD-style) + Command Line + Status Bar

#include "pch.h"
#include "framework.h"
#include "LargeHW.h"

#include "MainFrm.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

IMPLEMENT_DYNCREATE(CMainFrame, CFrameWnd)

BEGIN_MESSAGE_MAP(CMainFrame, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_SIZE()
END_MESSAGE_MAP()

static UINT indicators[] =
{
    ID_SEPARATOR,
    ID_INDICATOR_CAPS,
    ID_INDICATOR_NUM,
    ID_INDICATOR_SCRL,
};

CMainFrame::CMainFrame() noexcept
{
}

CMainFrame::~CMainFrame() {}

int CMainFrame::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
    if (CFrameWnd::OnCreate(lpCreateStruct) == -1)
        return -1;

    // 1. Toolbar with drawing commands
    if (!m_wndToolBar.CreateEx(this, TBSTYLE_FLAT,
        WS_CHILD | WS_VISIBLE | CBRS_TOP | CBRS_GRIPPER |
        CBRS_TOOLTIPS | CBRS_FLYBY | CBRS_SIZE_DYNAMIC) ||
        !m_wndToolBar.LoadToolBar(IDR_MAINFRAME))
    {
        TRACE0("Failed to create toolbar\n");
        return -1;
    }

    // Customize toolbar: add more buttons via code
    // The base toolbar has: New, Open, Save, Cut, Copy, Paste, Print, About
    // We'll extend it programmatically

    m_wndToolBar.EnableDocking(CBRS_ALIGN_ANY);
    EnableDocking(CBRS_ALIGN_ANY);
    DockControlBar(&m_wndToolBar);

    // Add CAD menus to the existing menu bar
    CMenu* pMenu = GetMenu();
    if (pMenu) {
        // Insert Draw menu before View (position 2)
        CMenu drawMenu;
        drawMenu.CreatePopupMenu();
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_LINE,      L"&Line");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_POLYLINE,  L"&Polyline");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_CIRCLE,    L"&Circle");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_ARC,       L"&Arc");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_RECTANGLE, L"&Rectangle");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_POLYGON,   L"P&olygon");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_ELLIPSE,   L"&Ellipse");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_TEXT,      L"&Text");
        pMenu->InsertMenu(2, MF_BYPOSITION | MF_POPUP,
                          (UINT_PTR)drawMenu.Detach(), L"&Draw");

        // Insert Modify menu after Draw (position 3)
        CMenu modifyMenu;
        modifyMenu.CreatePopupMenu();
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_MOVE,   L"&Move");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_COPY,   L"&Copy");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_ROTATE, L"&Rotate");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_SCALE,  L"&Scale");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_DELETE, L"&Erase");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_MIRROR, L"M&irror");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_OFFSET, L"&Offset");
        pMenu->InsertMenu(3, MF_BYPOSITION | MF_POPUP,
                          (UINT_PTR)modifyMenu.Detach(), L"&Modify");
    }

    // 2. Command line (bottom of window)
    CRect rcCmdLine(0, 0, 200, 24);
    if (!m_wndCmdLine.Create(WS_CHILD | WS_VISIBLE | WS_BORDER |
                             ES_LEFT | ES_AUTOHSCROLL,
                             rcCmdLine, this, IDC_COMMAND_LINE))
    {
        TRACE0("Failed to create command line\n");
    }
    m_fontCmdLine.CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    m_wndCmdLine.SetFont(&m_fontCmdLine);
    m_wndCmdLine.SetWindowText(L"Command: ");

    // 3. Status bar
    if (!m_wndStatusBar.Create(this))
    {
        TRACE0("Failed to create status bar\n");
        return -1;
    }
    m_wndStatusBar.SetIndicators(indicators,
                                 sizeof(indicators)/sizeof(UINT));

    return 0;
}

void CMainFrame::OnSize(UINT nType, int cx, int cy)
{
    CFrameWnd::OnSize(nType, cx, cy);

    if (m_wndCmdLine.GetSafeHwnd() && m_wndStatusBar.GetSafeHwnd()) {
        CRect rcStatus;
        m_wndStatusBar.GetWindowRect(&rcStatus);
        ScreenToClient(&rcStatus);
        int cmdLineHeight = 24;
        m_wndCmdLine.MoveWindow(0, rcStatus.top - cmdLineHeight,
                                cx, cmdLineHeight);
    }
}

void CMainFrame::SetStatusBarText(const CString& strText)
{
    if (m_wndStatusBar.GetSafeHwnd()) {
        m_wndStatusBar.SetPaneText(0, strText);
    }
}

BOOL CMainFrame::PreCreateWindow(CREATESTRUCT& cs)
{
    if (!CFrameWnd::PreCreateWindow(cs))
        return FALSE;
    cs.style &= ~FWS_ADDTOTITLE;
    cs.lpszName = L"Engineering CAD v1.0";
    return TRUE;
}

#ifdef _DEBUG
void CMainFrame::AssertValid() const { CFrameWnd::AssertValid(); }
void CMainFrame::Dump(CDumpContext& dc) const { CFrameWnd::Dump(dc); }
#endif
