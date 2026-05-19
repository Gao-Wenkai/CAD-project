// MainFrm.h: Main Frame - Toolbar + Command Line + Status Bar

#pragma once

class CMainFrame : public CFrameWnd
{
protected:
    CMainFrame() noexcept;
    DECLARE_DYNCREATE(CMainFrame)

public:
    CToolBar          m_wndToolBar;
    CStatusBar        m_wndStatusBar;
    void SetStatusBarText(const CString& strText);

    CEdit             m_wndCmdLine;
    CFont             m_fontCmdLine;
    CBrush            m_cmdLineBrush;

public:
    virtual BOOL PreCreateWindow(CREATESTRUCT& cs);
    virtual ~CMainFrame();

#ifdef _DEBUG
    virtual void AssertValid() const;
    virtual void Dump(CDumpContext& dc) const;
#endif

protected:
    afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    virtual BOOL PreTranslateMessage(MSG* pMsg) override;
    void ProcessCommandLine();

    DECLARE_MESSAGE_MAP()
};
