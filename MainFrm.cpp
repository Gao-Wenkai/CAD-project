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
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_LINE,      L"&Line\tL");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_POLYLINE,  L"&Polyline\tPL");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_CIRCLE,    L"&Circle\tC");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_ARC,       L"&Arc\tA");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_RECTANGLE, L"&Rectangle\tREC");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_POLYGON,   L"P&olygon\tPOL");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_ELLIPSE,   L"&Ellipse\tEL");
        drawMenu.AppendMenu(MF_STRING, ID_DRAW_TEXT,      L"&Text\tT");
        pMenu->InsertMenu(2, MF_BYPOSITION | MF_POPUP,
                          (UINT_PTR)drawMenu.Detach(), L"&Draw");

        // Insert Modify menu after Draw (position 3)
        CMenu modifyMenu;
        modifyMenu.CreatePopupMenu();
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_MOVE,   L"&Move\tM");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_COPY,   L"&Copy\tCO/CP");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_ROTATE, L"&Rotate\tRO");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_SCALE,  L"&Scale\tSC");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_DELETE, L"&Erase\tE/Del");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_MIRROR, L"M&irror\tMI");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_OFFSET, L"&Offset\tO");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_CHAMFER, L"&Chamfer\tCHA");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_FILLET, L"&Fillet\tF");
        modifyMenu.AppendMenu(MF_STRING, ID_MODIFY_ARRAY,  L"&Array\tAR");
        pMenu->InsertMenu(3, MF_BYPOSITION | MF_POPUP,
                          (UINT_PTR)modifyMenu.Detach(), L"&Modify");

        // Insert Format menu after Modify (position 4)
        CMenu formatMenu;
        formatMenu.CreatePopupMenu();
        formatMenu.AppendMenu(MF_STRING, ID_FORMAT_LAYER,    L"&Layer...");
        pMenu->InsertMenu(4, MF_BYPOSITION | MF_POPUP,
                          (UINT_PTR)formatMenu.Detach(), L"F&ormat");

        // Insert Script menu after Format (position 5)
        CMenu scriptMenu;
        scriptMenu.CreatePopupMenu();
        scriptMenu.AppendMenu(MF_STRING, ID_SCRIPT_RUN,          L"&Run Script...");
        scriptMenu.AppendMenu(MF_SEPARATOR);
        scriptMenu.AppendMenu(MF_STRING, ID_SCRIPT_RECORD_START, L"Start &Recording...");
        scriptMenu.AppendMenu(MF_STRING, ID_SCRIPT_RECORD_STOP,  L"Stop Re&cording");
        pMenu->InsertMenu(5, MF_BYPOSITION | MF_POPUP,
                          (UINT_PTR)scriptMenu.Detach(), L"&Script");
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

        if (RedirectPrintableKeyToCommandLine(pMsg))
            return TRUE;
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

bool CMainFrame::RedirectPrintableKeyToCommandLine(MSG* pMsg)
{
    if (!m_wndCmdLine.GetSafeHwnd())
        return false;

    CWnd* pFocus = GetFocus();
    if (pFocus && pFocus->GetSafeHwnd() == m_wndCmdLine.GetSafeHwnd())
        return false;

    if (::GetKeyState(VK_CONTROL) & 0x8000)
        return false;
    if (::GetKeyState(VK_MENU) & 0x8000)
        return false;

    UINT vk = (UINT)pMsg->wParam;
    if (vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_DELETE ||
        vk == VK_BACK || vk == VK_TAB ||
        (vk >= VK_F1 && vk <= VK_F24) ||
        vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
        vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
        vk == VK_INSERT) {
        return false;
    }

    BYTE keyboardState[256];
    if (!::GetKeyboardState(keyboardState))
        return false;

    WCHAR text[8] = {};
    UINT scanCode = ((UINT)pMsg->lParam >> 16) & 0xff;
    int nChars = ::ToUnicode(vk, scanCode, keyboardState, text, 7, 0);
    if (nChars <= 0)
        return false;

    CString typed(text, nChars);
    for (int i = 0; i < typed.GetLength(); ++i) {
        if (typed[i] < 0x20)
            return false;
    }

    CLargeHWView* pView = (CLargeHWView*)GetActiveView();
    CLargeHWDoc* pDoc = pView ? pView->GetDocument() : nullptr;
    CString prompt = pDoc ? pDoc->m_strCommandPrompt : L"Command: ";
    if (prompt.IsEmpty())
        prompt = L"Command: ";

    m_wndCmdLine.SetFocus();
    m_wndCmdLine.SetWindowText(prompt + typed);
    int nLen = prompt.GetLength() + typed.GetLength();
    m_wndCmdLine.SetSel(nLen, nLen);
    return true;
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

    pView->SubmitCommandLineInput(strCmd);
}

#ifdef _DEBUG
void CMainFrame::AssertValid() const { CFrameWnd::AssertValid(); }
void CMainFrame::Dump(CDumpContext& dc) const { CFrameWnd::Dump(dc); }
#endif
