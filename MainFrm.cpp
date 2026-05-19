// MainFrm.cpp: Main Frame Implementation
// Toolbar (AutoCAD-style) + Command Line + Status Bar

#include "pch.h"
#include "framework.h"
#include "LargeHW.h"

#include "MainFrm.h"
#include "LargeHWView.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

IMPLEMENT_DYNCREATE(CMainFrame, CFrameWnd)

BEGIN_MESSAGE_MAP(CMainFrame, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_SIZE()
    ON_WM_CTLCOLOR()
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
        // Insert "Redo" into the Edit menu (position 1, after Undo)
        CMenu* pEditMenu = pMenu->GetSubMenu(1);
        if (pEditMenu) {
            pEditMenu->InsertMenu(1, MF_BYPOSITION | MF_STRING, ID_EDIT_REDO, L"&Redo\tCtrl+Y");
        }

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

        // Insert Format menu after Modify (position 4)
        CMenu formatMenu;
        formatMenu.CreatePopupMenu();
        formatMenu.AppendMenu(MF_STRING, ID_FORMAT_LAYER,    L"&Layer...");
        pMenu->InsertMenu(4, MF_BYPOSITION | MF_POPUP,
                          (UINT_PTR)formatMenu.Detach(), L"F&ormat");
    }

    // 2. Command line (bottom of window) - AutoCAD-style
    CRect rcCmdLine(0, 0, 200, 60);
    if (!m_wndCmdLine.Create(WS_CHILD | WS_VISIBLE | WS_BORDER |
                             ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                             ES_WANTRETURN,
                             rcCmdLine, this, IDC_COMMAND_LINE))
    {
        TRACE0("Failed to create command line\n");
    }
    m_fontCmdLine.CreateFont(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    m_wndCmdLine.SetFont(&m_fontCmdLine);
    m_wndCmdLine.SetWindowText(L"Command: ");
    m_wndCmdLine.SetSel(lstrlen(L"Command: "), lstrlen(L"Command: "));
    m_wndCmdLine.SetMargins(8, 4);

    m_cmdLineBrush.CreateSolidBrush(RGB(40, 40, 44));

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
        int cmdLineHeight = 60;
        int cmdLineTop = rcStatus.top - cmdLineHeight;

        // Position command line at bottom, above status bar
        m_wndCmdLine.MoveWindow(0, rcStatus.top - cmdLineHeight,
                                cx, cmdLineHeight);

        // Resize view to not overlap command line
        CView* pView = GetActiveView();
        if (pView->GetSafeHwnd()) {
            CRect rcView;
            pView->GetWindowRect(&rcView);
            ScreenToClient(&rcView);
            rcView.bottom = cmdLineTop;
            pView->MoveWindow(&rcView);
        }
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

HBRUSH CMainFrame::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    HBRUSH hbr = CFrameWnd::OnCtlColor(pDC, pWnd, nCtlColor);

    if (pWnd->GetDlgCtrlID() == IDC_COMMAND_LINE) {
        pDC->SetTextColor(RGB(240, 240, 240));
        pDC->SetBkColor(RGB(40, 40, 44));
        return m_cmdLineBrush;
    }
    return hbr;
}

BOOL CMainFrame::PreTranslateMessage(MSG* pMsg)
{
    if (pMsg->message == WM_KEYDOWN) {
        CWnd* pFocus = GetFocus();
        if (pFocus && pFocus->GetSafeHwnd() == m_wndCmdLine.GetSafeHwnd()) {
            if (pMsg->wParam == VK_RETURN) {
                ProcessCommandLine();
                return TRUE;
            }
            if (pMsg->wParam == VK_ESCAPE) {
                CLargeHWView* pView = (CLargeHWView*)GetActiveView();
                if (pView) pView->OnCancelCommand();
                return TRUE;
            }
        }
    }

    // Fix cursor when user clicks in command line - ensure it's after prompt
    if (pMsg->message == WM_LBUTTONDOWN && pMsg->hwnd == m_wndCmdLine.GetSafeHwnd()) {
        PostMessage(WM_APP + 1);
    }
    if (pMsg->message == WM_APP + 1) {
        CLargeHWView* pView = (CLargeHWView*)GetActiveView();
        if (pView && pView->GetDocument()) {
            int nLen = pView->GetDocument()->m_strCommandPrompt.GetLength();
            int nStart, nEnd;
            m_wndCmdLine.GetSel(nStart, nEnd);
            if (nStart < nLen || nEnd < nLen) {
                m_wndCmdLine.SetSel(nLen, max(nEnd, nLen));
            }
        }
        return TRUE;
    }

    return CFrameWnd::PreTranslateMessage(pMsg);
}

void CMainFrame::ProcessCommandLine()
{
    CLargeHWView* pView = (CLargeHWView*)GetActiveView();
    if (!pView) return;

    CString strRaw;
    m_wndCmdLine.GetWindowText(strRaw);
    strRaw.Trim();

    if (strRaw.IsEmpty()) return;

    // Remove the prompt prefix wherever it appears in the string
    CLargeHWDoc* pDoc = pView->GetDocument();
    CString strCmd;
    if (pDoc) {
        CString strPrompt = pDoc->m_strCommandPrompt;
        int nLen = strPrompt.GetLength();
        if (nLen > 0) {
            // Take text after the LAST occurrence of the prompt
            // (user might have typed before the prompt if cursor was misplaced)
            int nPos = strRaw.Find(strPrompt);
            if (nPos >= 0) {
                // Extract everything after the prompt
                strCmd = strRaw.Mid(nPos + nLen);
            } else {
                // No prompt found - user deleted the prompt, use entire text
                strCmd = strRaw;
            }
        } else {
            strCmd = strRaw;
        }
    } else {
        strCmd = strRaw;
    }
    strCmd.Trim();

    if (strCmd.IsEmpty()) {
        pView->SendMessage(WM_KEYDOWN, VK_RETURN, 0);
    }
    // Coordinate input: comma, @, <, or single number while in a point-entry state
    else if (strCmd.Find(L',') >= 0 || strCmd[0] == L'@' || strCmd.Find(L'<') >= 0 ||
             (pDoc && pDoc->m_drawState != STATE_IDLE && _wtoi(strCmd) != 0 && strCmd != L"0")) {
        pView->ProcessCoordinateInput(strCmd);
    }
    // Command input
    else {
        pView->ExecuteCommand(strCmd);
    }

    // Reset to default prompt if still in idle state
    if (pDoc && pDoc->m_drawState == STATE_IDLE) {
        pDoc->m_strCommandPrompt = L"Command: ";
    }

    // Reset command line text + cursor at end
    CString strPrompt;
    if (pDoc) strPrompt = pDoc->m_strCommandPrompt;
    if (strPrompt.IsEmpty()) strPrompt = L"Command: ";
    m_wndCmdLine.SetWindowText(strPrompt);
    m_wndCmdLine.SetSel(strPrompt.GetLength(), strPrompt.GetLength());
}

#ifdef _DEBUG
void CMainFrame::AssertValid() const { CFrameWnd::AssertValid(); }
void CMainFrame::Dump(CDumpContext& dc) const { CFrameWnd::Dump(dc); }
#endif
