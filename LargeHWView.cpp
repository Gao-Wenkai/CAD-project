// LargeHWView.cpp: CAD View Implementation
// Rendering engine (AutoCAD-style dark background/grid/crosshair)
// Interaction state machine (simulates AutoCAD command-line prompt flow)

#include "pch.h"
#include "framework.h"
#ifndef SHARED_HANDLERS
#include "LargeHW.h"
#endif

#include "LargeHWDoc.h"
#include "LargeHWView.h"
#include "MainFrm.h"

#include <afxdlgs.h>
#include <cstdlib>
#include <cwctype>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
CStringA WideToUtf8(const CString& text)
{
    int nChars = text.GetLength();
    if (nChars == 0) return CStringA();

    int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, text, nChars, nullptr, 0, nullptr, nullptr);
    if (nBytes <= 0) return CStringA();

    CStringA utf8;
    LPSTR pBuffer = utf8.GetBuffer(nBytes);
    ::WideCharToMultiByte(CP_UTF8, 0, text, nChars, pBuffer, nBytes, nullptr, nullptr);
    utf8.ReleaseBuffer(nBytes);
    return utf8;
}

bool DecodeTextBytes(const std::vector<BYTE>& bytes, CString& text)
{
    text.Empty();
    if (bytes.empty()) return true;

    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        int nChars = (int)((bytes.size() - 2) / sizeof(wchar_t));
        text = CString((LPCWSTR)(bytes.data() + 2), nChars);
        return true;
    }

    int nOffset = 0;
    UINT nCodePage = CP_UTF8;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        nOffset = 3;
    }

    int nSize = (int)bytes.size() - nOffset;
    if (nSize <= 0) return true;

    int nChars = ::MultiByteToWideChar(nCodePage, MB_ERR_INVALID_CHARS,
                                       (LPCCH)bytes.data() + nOffset, nSize,
                                       nullptr, 0);
    if (nChars <= 0) {
        nCodePage = CP_ACP;
        nChars = ::MultiByteToWideChar(nCodePage, 0,
                                       (LPCCH)bytes.data(), (int)bytes.size(),
                                       nullptr, 0);
        nOffset = 0;
        nSize = (int)bytes.size();
    }

    if (nChars <= 0) return false;

    LPWSTR pBuffer = text.GetBuffer(nChars);
    ::MultiByteToWideChar(nCodePage, 0, (LPCCH)bytes.data() + nOffset, nSize, pBuffer, nChars);
    text.ReleaseBuffer(nChars);
    return true;
}

bool LoadTextFile(const CString& path, CString& text)
{
    CFile file;
    if (!file.Open(path, CFile::modeRead | CFile::shareDenyWrite))
        return false;

    ULONGLONG nLen64 = file.GetLength();
    if (nLen64 > INT_MAX) {
        file.Close();
        return false;
    }

    std::vector<BYTE> bytes((size_t)nLen64);
    if (!bytes.empty())
        file.Read(bytes.data(), (UINT)bytes.size());
    file.Close();

    return DecodeTextBytes(bytes, text);
}
}

IMPLEMENT_DYNCREATE(CLargeHWView, CView)

// Text input dialog
class CTextInputDlg : public CDialog
{
public:
    CString m_strText;
    int     m_nHeight;

    CTextInputDlg(CWnd* pParent = nullptr)
        : CDialog(IDD_TEXT_INPUT, pParent)
        , m_strText(_T(""))
        , m_nHeight(20)
    {}

protected:
    virtual BOOL OnInitDialog() override
    {
        CDialog::OnInitDialog();
        SetDlgItemText(IDC_TEXT_CONTENT, L"");
        SetDlgItemInt(IDC_TEXT_HEIGHT, 20, FALSE);
        GetDlgItem(IDC_TEXT_CONTENT)->SetFocus();
        return FALSE;
    }

    virtual void OnOK() override
    {
        GetDlgItemText(IDC_TEXT_CONTENT, m_strText);
        m_nHeight = GetDlgItemInt(IDC_TEXT_HEIGHT, NULL, FALSE);
        if (m_nHeight < 1) m_nHeight = 20;
        if (m_strText.IsEmpty())
            m_strText = L"Text";
        CDialog::OnOK();
    }

    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CTextInputDlg, CDialog)
END_MESSAGE_MAP()

BEGIN_MESSAGE_MAP(CLargeHWView, CView)
    ON_COMMAND(ID_FILE_PRINT, &CView::OnFilePrint)
    ON_COMMAND(ID_FILE_PRINT_DIRECT, &CView::OnFilePrint)
    ON_COMMAND(ID_FILE_PRINT_PREVIEW, &CView::OnFilePrintPreview)

    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSEMOVE()
    ON_WM_RBUTTONDOWN()
    ON_WM_MBUTTONDOWN()
    ON_WM_MBUTTONUP()
    ON_WM_MOUSEWHEEL()
    ON_WM_KEYDOWN()
    ON_WM_SETCURSOR()
    ON_WM_SIZE()
    ON_WM_CONTEXTMENU()

    ON_COMMAND(ID_DRAW_LINE,       &CLargeHWView::OnDrawLine)
    ON_COMMAND(ID_DRAW_POLYLINE,   &CLargeHWView::OnDrawPolyline)
    ON_COMMAND(ID_DRAW_CIRCLE,     &CLargeHWView::OnDrawCircle)
    ON_COMMAND(ID_DRAW_ARC,        &CLargeHWView::OnDrawArc)
    ON_COMMAND(ID_DRAW_RECTANGLE,  &CLargeHWView::OnDrawRectangle)
    ON_COMMAND(ID_DRAW_POLYGON,    &CLargeHWView::OnDrawPolygon)
    ON_COMMAND(ID_DRAW_ELLIPSE,    &CLargeHWView::OnDrawEllipse)
    ON_COMMAND(ID_DRAW_TEXT,       &CLargeHWView::OnDrawText)

    ON_COMMAND(ID_MODIFY_MOVE,     &CLargeHWView::OnModifyMove)
    ON_COMMAND(ID_MODIFY_COPY,     &CLargeHWView::OnModifyCopy)
    ON_COMMAND(ID_MODIFY_ROTATE,   &CLargeHWView::OnModifyRotate)
    ON_COMMAND(ID_MODIFY_SCALE,    &CLargeHWView::OnModifyScale)
    ON_COMMAND(ID_MODIFY_DELETE,   &CLargeHWView::OnModifyDelete)
    ON_COMMAND(ID_MODIFY_MIRROR,   &CLargeHWView::OnModifyMirror)
    ON_COMMAND(ID_MODIFY_OFFSET,   &CLargeHWView::OnModifyOffset)

    ON_COMMAND(ID_EDIT_UNDO,       &CLargeHWView::OnEditUndo)
    ON_COMMAND(ID_EDIT_REDO,       &CLargeHWView::OnEditRedo)
    ON_COMMAND(ID_CAD_UNDO,        &CLargeHWView::OnEditUndo)
    ON_COMMAND(ID_CAD_REDO,        &CLargeHWView::OnEditRedo)
    ON_COMMAND(ID_EDIT_SELECTALL,  &CLargeHWView::OnEditSelectAll)

    ON_UPDATE_COMMAND_UI(ID_EDIT_UNDO, &CLargeHWView::OnUpdateEditUndo)
    ON_UPDATE_COMMAND_UI(ID_EDIT_REDO, &CLargeHWView::OnUpdateEditRedo)

    ON_COMMAND(ID_VIEW_ZOOM_EXTENTS,&CLargeHWView::OnViewZoomExtents)
    ON_COMMAND(ID_VIEW_ZOOM_WINDOW, &CLargeHWView::OnViewZoomWindow)
    ON_COMMAND(ID_VIEW_PAN,         &CLargeHWView::OnViewPan)
    ON_COMMAND(ID_VIEW_GRID,        &CLargeHWView::OnViewGrid)
    ON_COMMAND(ID_VIEW_SNAP,        &CLargeHWView::OnViewSnap)
    ON_COMMAND(ID_VIEW_ORTHO,       &CLargeHWView::OnViewOrtho)

    ON_COMMAND(ID_COLOR_RED,       &CLargeHWView::OnColorRed)
    ON_COMMAND(ID_COLOR_YELLOW,    &CLargeHWView::OnColorYellow)
    ON_COMMAND(ID_COLOR_GREEN,     &CLargeHWView::OnColorGreen)
    ON_COMMAND(ID_COLOR_CYAN,      &CLargeHWView::OnColorCyan)
    ON_COMMAND(ID_COLOR_BLUE,      &CLargeHWView::OnColorBlue)
    ON_COMMAND(ID_COLOR_MAGENTA,   &CLargeHWView::OnColorMagenta)
    ON_COMMAND(ID_COLOR_WHITE,     &CLargeHWView::OnColorWhite)
    ON_COMMAND(ID_LINETYPE_SOLID,  &CLargeHWView::OnLinetypeSolid)
    ON_COMMAND(ID_LINETYPE_DASH,   &CLargeHWView::OnLinetypeDash)
    ON_COMMAND(ID_LINETYPE_DOT,    &CLargeHWView::OnLinetypeDot)
    ON_COMMAND(ID_LINETYPE_DASHDOT,&CLargeHWView::OnLinetypeDashDot)
    ON_COMMAND(ID_LINEWEIGHT_1,    &CLargeHWView::OnLineweight1)
    ON_COMMAND(ID_LINEWEIGHT_2,    &CLargeHWView::OnLineweight2)
    ON_COMMAND(ID_LINEWEIGHT_3,    &CLargeHWView::OnLineweight3)
    ON_COMMAND(ID_LINEWEIGHT_4,    &CLargeHWView::OnLineweight4)

    ON_COMMAND(ID_CONTEXT_CANCEL,  &CLargeHWView::OnCancelCommand)
    ON_COMMAND(ID_CONTEXT_REPEAT,  &CLargeHWView::OnContextRepeat)
    ON_COMMAND(ID_FORMAT_LAYER,    &CLargeHWView::OnFormatLayer)
    ON_COMMAND(ID_SCRIPT_RUN,      &CLargeHWView::OnScriptRun)
    ON_COMMAND(ID_SCRIPT_RECORD_START, &CLargeHWView::OnScriptRecordStart)
    ON_COMMAND(ID_SCRIPT_RECORD_STOP,  &CLargeHWView::OnScriptRecordStop)
    ON_UPDATE_COMMAND_UI(ID_SCRIPT_RUN, &CLargeHWView::OnUpdateScriptRun)
    ON_UPDATE_COMMAND_UI(ID_SCRIPT_RECORD_START, &CLargeHWView::OnUpdateScriptRecordStart)
    ON_UPDATE_COMMAND_UI(ID_SCRIPT_RECORD_STOP,  &CLargeHWView::OnUpdateScriptRecordStop)
END_MESSAGE_MAP()

// Constructor/Destructor
CLargeHWView::CLargeHWView() noexcept
    : m_bDrawing(false)
    , m_bDragging(false)
    , m_ptDragStart(0, 0)
    , m_nGripIndex(-1)
    , m_pGripEntity(nullptr)
    , m_nPolygonSides(6)
    , m_bArcAltHalf(false)
    , m_bPolylineClose(false)
    , m_bPanning(false)
    , m_ptPanStart(0, 0)
    , m_ptPanOffsetStart(0, 0)
    , m_ptSnapped(0, 0)
    , m_bSnapActive(false)
    , m_nSnapType(SNAP_NONE)
    , m_nLastCommandID(0)
    , m_currentColor(RGB(0, 255, 209))
    , m_currentLineStyle(PS_SOLID)
    , m_currentLineWidth(1)
    , m_bScriptRecording(false)
    , m_bRunningScript(false)
    , m_bSubmittingCommandLine(false)
{
}

CLargeHWView::~CLargeHWView()
{
    if (m_bScriptRecording) {
        m_scriptRecordFile.Close();
        m_bScriptRecording = false;
    }
}

BOOL CLargeHWView::PreCreateWindow(CREATESTRUCT& cs)
{
    return CView::PreCreateWindow(cs);
}

// ============================================================
// OnDraw - Main render (AutoCAD-style dark model space)
// ============================================================
void CLargeHWView::OnDraw(CDC* pDC)
{
    CLargeHWDoc* pDoc = GetDocument();
    ASSERT_VALID(pDoc);
    if (!pDoc) return;

    CRect rcClient;
    GetClientRect(&rcClient);

    // Dark background (AutoCAD model space: #1A1A1A)
    COLORREF bgColor = RGB(26, 26, 26);
    pDC->FillSolidRect(&rcClient, bgColor);

    // Grid
    if (pDoc->m_bShowGrid) DrawGrid(pDC);

    // Border
    CPen borderPen(PS_SOLID, 1, RGB(60, 60, 60));
    CPen* pOldPen = pDC->SelectObject(&borderPen);
    CBrush* pOldBrush = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
    pDC->Rectangle(&rcClient);

    // Entities
    DrawEntities(pDC);

    static int drawCount = 0;
    drawCount++;
    if (drawCount % 30 == 1)
        TRACE(L"[DEBUG] OnDraw #%d: entCount=%d, state=%d, scale=%.2f, offset=(%d,%d)\n",
              drawCount, (int)pDoc->GetEntities().size(), (int)pDoc->m_drawState, pDoc->m_dScale,
              pDoc->m_ptOffset.x, pDoc->m_ptOffset.y);

    // Preview (rubber band)
    if (m_bDrawing) DrawPreview(pDC);

    // UCS icon
    DrawUCSIcon(pDC);

    // Snap marker
    if (m_bSnapActive)
        DrawSnapMarker(pDC);

    // Crosshair cursor
    CPoint cursorPt;
    GetCursorPos(&cursorPt);
    ScreenToClient(&cursorPt);
    if (rcClient.PtInRect(cursorPt))
        DrawCrosshair(pDC, cursorPt);

    pDC->SelectObject(pOldPen);
    pDC->SelectObject(pOldBrush);
}

// ============================================================
// Draw grid (AutoCAD-style light gray dotted grid)
// ============================================================
void CLargeHWView::DrawGrid(CDC* pDC)
{
    CLargeHWDoc* pDoc = GetDocument();
    CRect rcClient;
    GetClientRect(&rcClient);

    int spacing = pDoc->m_nGridSpacing;
    int scaledSpacing = (int)(spacing * pDoc->m_dScale);
    if (scaledSpacing < 10) scaledSpacing = spacing;

    int offX = pDoc->m_ptOffset.x % scaledSpacing;
    int offY = pDoc->m_ptOffset.y % scaledSpacing;

    if (scaledSpacing < 5) return;

    CPen gridPen(PS_DOT, 0, RGB(40, 40, 40));
    CPen* pOldPen = pDC->SelectObject(&gridPen);

    for (int x = offX; x < rcClient.right; x += scaledSpacing) {
        pDC->MoveTo(x, 0);
        pDC->LineTo(x, rcClient.bottom);
    }
    for (int y = offY; y < rcClient.bottom; y += scaledSpacing) {
        pDC->MoveTo(0, y);
        pDC->LineTo(rcClient.right, y);
    }

    pDC->SelectObject(pOldPen);
}

// ============================================================
// Crosshair cursor (AutoCAD-style)
// ============================================================
void CLargeHWView::DrawCrosshair(CDC* pDC, CPoint pt)
{
    CRect rcClient;
    GetClientRect(&rcClient);

    CPen crossPen(PS_SOLID, 1, RGB(200, 200, 200));
    CPen* pOldPen = pDC->SelectObject(&crossPen);
    int oldROP = pDC->SetROP2(R2_XORPEN);

    pDC->MoveTo(0, pt.y);
    pDC->LineTo(rcClient.right, pt.y);
    pDC->MoveTo(pt.x, 0);
    pDC->LineTo(pt.x, rcClient.bottom);

    // Center box (5x5)
    pDC->MoveTo(pt.x - 5, pt.y - 5);
    pDC->LineTo(pt.x + 5, pt.y - 5);
    pDC->LineTo(pt.x + 5, pt.y + 5);
    pDC->LineTo(pt.x - 5, pt.y + 5);
    pDC->LineTo(pt.x - 5, pt.y - 5);

    pDC->SetROP2(oldROP);
    pDC->SelectObject(pOldPen);
}

// ============================================================
// Draw all entities
// ============================================================
void CLargeHWView::DrawEntities(CDC* pDC)
{
    CLargeHWDoc* pDoc = GetDocument();
    const auto& ents = pDoc->GetEntities();
    static int entTraceCount = 0;
    entTraceCount++;
    if (entTraceCount % 30 == 1)
        TRACE(L"[DEBUG] DrawEntities #%d: entCount=%d\n", entTraceCount, (int)ents.size());
    for (size_t i = 0; i < ents.size(); ++i) {
        CEntity* p = const_cast<CEntity*>(ents[i]);
        p->Draw(pDC, pDoc->m_dScale, pDoc->m_ptOffset);
        if (p->m_bSelected) {
            CRect bounds = p->GetBounds();
            CRect screenBounds(
                (int)(bounds.left * pDoc->m_dScale + pDoc->m_ptOffset.x),
                (int)(bounds.top * pDoc->m_dScale + pDoc->m_ptOffset.y),
                (int)(bounds.right * pDoc->m_dScale + pDoc->m_ptOffset.x),
                (int)(bounds.bottom * pDoc->m_dScale + pDoc->m_ptOffset.y)
            );
            screenBounds.NormalizeRect();
            screenBounds.InflateRect(3, 3);
            CPen hlPen(PS_SOLID, 2, RGB(0, 255, 255));
            CPen* pOld = pDC->SelectObject(&hlPen);
            CBrush* pOldBr = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
            pDC->Rectangle(&screenBounds);
            pDC->SelectObject(pOld);
            pDC->SelectObject(pOldBr);
        }
    }
}

// ============================================================
// Draw preview (rubber band)
// ============================================================
void CLargeHWView::DrawPreview(CDC* pDC)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (m_tempPts.empty()) return;

    CPen previewPen(PS_DASH, 1, RGB(150, 150, 150));
    CPen* pOldPen = pDC->SelectObject(&previewPen);
    int oldROP = pDC->SetROP2(R2_NOTXORPEN);

    CPoint curWorld = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
    CPoint cursorPt = m_bSnapActive ? m_ptSnapped : m_ptCurrent;

    switch (pDoc->m_drawState) {
    case STATE_DRAW_LINE_P2:
    case STATE_DRAW_RECT_P2:
        if (m_tempPts.size() >= 1) {
            CPoint p1 = WorldToScreen(m_tempPts[0]);
            pDC->MoveTo(p1);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_DRAW_CIRCLE_RADIUS:
        if (m_tempPts.size() >= 1) {
            CPoint c = WorldToScreen(m_tempPts[0]);
            int r = (int)(Distance(m_tempPts[0], curWorld) * pDoc->m_dScale);
            CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
            pDC->Ellipse(c.x - r, c.y - r, c.x + r, c.y + r);
            pDC->SelectObject(pB);
        }
        break;

    case STATE_DRAW_ELLIPSE_RADIUS:
        if (m_tempPts.size() >= 1) {
            CPoint c = WorldToScreen(m_tempPts[0]);
            int rx = (int)(abs(curWorld.x - m_tempPts[0].x) * pDoc->m_dScale);
            int ry = (int)(abs(curWorld.y - m_tempPts[0].y) * pDoc->m_dScale);
            CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
            pDC->Ellipse(c.x - rx, c.y - ry, c.x + rx, c.y + ry);
            pDC->SelectObject(pB);
        }
        break;

    case STATE_DRAW_POLYGON_RADIUS:
        if (m_tempPts.size() >= 1) {
            int r = (int)(Distance(m_tempPts[0], curWorld) * pDoc->m_dScale);
            CPoint c = WorldToScreen(m_tempPts[0]);
            double step = 2.0 * M_PI / m_nPolygonSides;
            for (int i = 0; i < m_nPolygonSides; ++i) {
                double a1 = -M_PI/2 + step * i;
                double a2 = -M_PI/2 + step * (i+1);
                pDC->MoveTo(c.x + (int)(r*cos(a1)), c.y + (int)(r*sin(a1)));
                pDC->LineTo(c.x + (int)(r*cos(a2)), c.y + (int)(r*sin(a2)));
            }
        }
        break;

    case STATE_DRAW_POLYLINE_POINT:
        // Draw all placed segments + rubber band to cursor
        if (m_tempPts.size() >= 2) {
            for (size_t i = 0; i < m_tempPts.size() - 1; ++i) {
                CPoint p1 = WorldToScreen(m_tempPts[i]);
                CPoint p2 = WorldToScreen(m_tempPts[i+1]);
                pDC->MoveTo(p1);
                pDC->LineTo(p2);
            }
        }
        if (m_tempPts.size() >= 1) {
            CPoint last = WorldToScreen(m_tempPts.back());
            pDC->MoveTo(last);
            pDC->LineTo(cursorPt);
        }
        // Closed mode: dashed line from last point back to first
        if (m_bPolylineClose && m_tempPts.size() >= 2) {
            CPoint first = WorldToScreen(m_tempPts.front());
            CPoint last  = WorldToScreen(m_tempPts.back());
            pDC->MoveTo(last);
            pDC->LineTo(first);
        }
        break;

    case STATE_DRAW_ARC_P3:
        if (m_tempPts.size() >= 2) {
            CPoint ptStart  = m_tempPts[0];
            CPoint ptCenter = m_tempPts[1];
            int r = (int)Distance(ptCenter, ptStart);
            if (r > 0) {
                CPoint c = WorldToScreen(ptCenter);
                int scrR = (int)(r * pDoc->m_dScale);
                double angS = atan2((double)(ptStart.y - ptCenter.y),
                                    (double)(ptStart.x - ptCenter.x));
                double angE = atan2((double)(curWorld.y - ptCenter.y),
                                    (double)(curWorld.x - ptCenter.x));

                CPoint scrStart(c.x + (int)(scrR * cos(angS)),
                                c.y + (int)(scrR * sin(angS)));
                CPoint scrEnd(c.x + (int)(scrR * cos(angE)),
                              c.y + (int)(scrR * sin(angE)));

                pDC->Arc(c.x - scrR, c.y - scrR, c.x + scrR, c.y + scrR,
                         scrStart.x, scrStart.y, scrEnd.x, scrEnd.y);
            }
        }
        break;

    case STATE_DRAW_ARC_PREVIEW:
        if (m_tempPts.size() >= 3) {
            CPoint ptStart  = m_tempPts[0];
            CPoint ptCenter = m_tempPts[1];
            CPoint ptEnd    = m_tempPts[2];
            int r = (int)Distance(ptCenter, ptStart);
            if (r > 0) {
                CPoint c = WorldToScreen(ptCenter);
                int scrR = (int)(r * pDoc->m_dScale);
                double angS = atan2((double)(ptStart.y - ptCenter.y),
                                    (double)(ptStart.x - ptCenter.x));
                double angE = atan2((double)(ptEnd.y - ptCenter.y),
                                    (double)(ptEnd.x - ptCenter.x));

                CPoint scrStart(c.x + (int)(scrR * cos(angS)),
                                c.y + (int)(scrR * sin(angS)));
                CPoint scrEnd(c.x + (int)(scrR * cos(angE)),
                              c.y + (int)(scrR * sin(angE)));

                if (!m_bArcAltHalf) {
                    pDC->Arc(c.x - scrR, c.y - scrR, c.x + scrR, c.y + scrR,
                             scrStart.x, scrStart.y, scrEnd.x, scrEnd.y);
                } else {
                    // Draw complementary arc: swap start/end
                    pDC->Arc(c.x - scrR, c.y - scrR, c.x + scrR, c.y + scrR,
                             scrEnd.x, scrEnd.y, scrStart.x, scrStart.y);
                }
            }
        }
        break;

    case STATE_MOVE_DEST:
    case STATE_COPY_DEST:
        if (m_tempPts.size() >= 1) {
            CPoint base = WorldToScreen(m_tempPts.back());
            TRACE(L"[DEBUG] DrawPreview MOVE/COPY_DEST: base=(%d,%d), cursor=(%d,%d), tempPts=%d\n",
                  base.x, base.y, cursorPt.x, cursorPt.y, (int)m_tempPts.size());
            pDC->MoveTo(base);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_WINDOW_SELECT: {
        CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
        pDC->Rectangle(m_ptDragStart.x, m_ptDragStart.y, m_ptCurrent.x, m_ptCurrent.y);
        pDC->SelectObject(pB);
        break;
    }

    case STATE_ROTATE_ANGLE:
        if (m_tempPts.size() >= 1) {
            CPoint base = WorldToScreen(m_tempPts[0]);
            pDC->MoveTo(base);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_SCALE_FACTOR:
        if (m_tempPts.size() >= 1) {
            CPoint base = WorldToScreen(m_tempPts[0]);
            pDC->MoveTo(base);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_MIRROR_P1:
        // No preview needed - first click sets mirror line start
        break;

    case STATE_MIRROR_P2:
        if (m_tempPts.size() >= 1) {
            CPoint p1 = WorldToScreen(m_tempPts[0]);
            CPoint cursorPt = m_bSnapActive ? m_ptSnapped : m_ptCurrent;
            pDC->MoveTo(p1);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_ZOOM_WINDOW_P1:
        if (m_bDragging) {
            CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
            pDC->Rectangle(m_ptDragStart.x, m_ptDragStart.y, m_ptCurrent.x, m_ptCurrent.y);
            pDC->SelectObject(pB);
        } else {
            pDC->MoveTo(m_ptDragStart);
            pDC->LineTo(m_ptCurrent);
            pDC->MoveTo(m_ptCurrent);
            pDC->LineTo(m_ptDragStart.x, m_ptCurrent.y);
            pDC->MoveTo(m_ptCurrent);
            pDC->LineTo(m_ptCurrent.x, m_ptDragStart.y);
        }
        break;

    case STATE_ZOOM_WINDOW_P2: {
        CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
        pDC->Rectangle(m_ptDragStart.x, m_ptDragStart.y, m_ptCurrent.x, m_ptCurrent.y);
        pDC->SelectObject(pB);
        break;
    }
    }

    pDC->SetROP2(oldROP);
    pDC->SelectObject(pOldPen);
}

// ============================================================
// Draw UCS icon (bottom-left X/Y arrows)
// ============================================================
void CLargeHWView::DrawUCSIcon(CDC* pDC)
{
    CRect rcClient;
    GetClientRect(&rcClient);

    int cx = 40, cy = rcClient.bottom - 40;
    CPen xPen(PS_SOLID, 2, RGB(255, 0, 0));
    CPen yPen(PS_SOLID, 2, RGB(0, 255, 0));
    CPen* pOld = pDC->SelectObject(&xPen);

    // X axis
    pDC->MoveTo(cx, cy);
    pDC->LineTo(cx + 30, cy);
    pDC->LineTo(cx + 25, cy - 5);
    pDC->MoveTo(cx + 30, cy);
    pDC->LineTo(cx + 25, cy + 5);
    pDC->SetTextColor(RGB(255, 100, 100));
    pDC->TextOutW(cx + 32, cy - 8, L"X");

    // Y axis
    pDC->SelectObject(&yPen);
    pDC->MoveTo(cx, cy);
    pDC->LineTo(cx, cy - 30);
    pDC->LineTo(cx - 5, cy - 25);
    pDC->MoveTo(cx, cy - 30);
    pDC->LineTo(cx + 5, cy - 25);
    pDC->SetTextColor(RGB(100, 255, 100));
    pDC->TextOutW(cx - 15, cy - 32, L"Y");

    pDC->SelectObject(pOld);
}

// ============================================================
// Draw snap marker
// ============================================================
void CLargeHWView::DrawSnapMarker(CDC* pDC)
{
    CPoint scr = m_ptSnapped;
    COLORREF markerColor;
    switch (m_nSnapType) {
    case SNAP_ENDPOINT:  markerColor = RGB(255, 128, 0); break;
    case SNAP_MIDPOINT:  markerColor = RGB(0, 255, 128); break;
    case SNAP_CENTER:    markerColor = RGB(255, 255, 0); break;
    case SNAP_QUADRANT:  markerColor = RGB(0, 255, 255); break;
    case SNAP_NEAREST:   markerColor = RGB(200, 200, 200); break;
    default:             markerColor = RGB(128, 255, 0); break;
    }

    CPen snapPen(PS_SOLID, 2, markerColor);
    CPen* pOldPen = pDC->SelectObject(&snapPen);
    CBrush* pOldBrush = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);

    int sz = 8;
    pDC->Rectangle(scr.x - sz, scr.y - sz, scr.x + sz, scr.y + sz);
    pDC->MoveTo(scr.x - sz - 2, scr.y);
    pDC->LineTo(scr.x + sz + 2, scr.y);
    pDC->MoveTo(scr.x, scr.y - sz - 2);
    pDC->LineTo(scr.x, scr.y + sz + 2);

    pDC->SelectObject(pOldPen);
    pDC->SelectObject(pOldBrush);
}

// ============================================================
// Coordinate transforms
// ============================================================
CPoint CLargeHWView::WorldToScreen(CPoint world) const
{
    CLargeHWDoc* pDoc = GetDocument();
    double s = pDoc ? pDoc->m_dScale : 1.0;
    CPoint off = pDoc ? pDoc->m_ptOffset : CPoint(0, 0);
    return CPoint((int)(world.x * s + off.x), (int)(world.y * s + off.y));
}

CPoint CLargeHWView::ScreenToWorld(CPoint screen) const
{
    CLargeHWDoc* pDoc = GetDocument();
    double s = pDoc ? pDoc->m_dScale : 1.0;
    if (s <= 0) s = 1;
    CPoint off = pDoc ? pDoc->m_ptOffset : CPoint(0, 0);
    return CPoint((int)((screen.x - off.x) / s), (int)((screen.y - off.y) / s));
}

CPoint CLargeHWView::SnapToGrid(CPoint pt) const
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || !pDoc->m_bSnapToGrid) return pt;
    int gs = pDoc->m_nGridSpacing;
    int x = ((pt.x + gs / 2) / gs) * gs;
    int y = ((pt.y + gs / 2) / gs) * gs;
    return CPoint(x, y);
}

CPoint CLargeHWView::SnapToObjects(CPoint pt)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || !pDoc->m_bObjectSnap) return pt;

    std::vector<CPoint> snapPts;
    std::vector<SnapType> snapTypes;
    pDoc->CollectSnapPoints(snapPts, snapTypes);

    double screenPt = 12.0 / pDoc->m_dScale;
    if (screenPt < 1.0) screenPt = 1.0;
    double bestDist = screenPt;
    CPoint bestPt = pt;
    SnapType bestType = SNAP_NONE;

    for (size_t i = 0; i < snapPts.size(); ++i) {
        SnapType t = snapTypes[i];
        if (!pDoc->m_bSnapEndpoint && t == SNAP_ENDPOINT) continue;
        if (!pDoc->m_bSnapMidpoint && t == SNAP_MIDPOINT) continue;
        if (!pDoc->m_bSnapCenter && t == SNAP_CENTER) continue;
        if (!pDoc->m_bSnapQuadrant && t == SNAP_QUADRANT) continue;
        if (!pDoc->m_bSnapNearest && t == SNAP_NEAREST) continue;

        double d = Distance(pt, snapPts[i]);
        if (d < bestDist) {
            bestDist = d;
            bestPt = snapPts[i];
            bestType = t;
        }
    }

    if (bestType != SNAP_NONE) {
        m_nSnapType = bestType;
        return bestPt;
    }

    return pt;
}

CPoint CLargeHWView::ApplyOrtho(CPoint pt, CPoint ref) const
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || !pDoc->m_bOrthoMode) return pt;
    double dx = fabs((double)(pt.x - ref.x));
    double dy = fabs((double)(pt.y - ref.y));
    if (dx >= dy)
        return CPoint(pt.x, ref.y);
    else
        return CPoint(ref.x, pt.y);
}

// ============================================================
// Update status bar (coordinates + command prompt)
// ============================================================
void CLargeHWView::UpdateStatusBar()
{
    CLargeHWDoc* pDoc = GetDocument();
    CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
    if (!pFrame || !pDoc) return;

    CPoint world = ScreenToWorld(m_ptCurrent);
    CString strCoord;
    strCoord.Format(L"X: %d  Y: %d  | Zoom: %.2f  |  %s  SNAP=%s GRID=%s ORTHO=%s OSNAP=%s",
                    world.x, world.y, pDoc->m_dScale,
                    (LPCTSTR)pDoc->m_strCommandPrompt,
                    pDoc->m_bSnapToGrid ? L"ON" : L"OFF",
                    pDoc->m_bShowGrid ? L"ON" : L"OFF",
                    pDoc->m_bOrthoMode ? L"ON" : L"OFF",
                    pDoc->m_bObjectSnap ? L"ON" : L"OFF");

    pFrame->SetStatusBarText(strCoord);
}

// ============================================================
// Set drawing state
// ============================================================
void CLargeHWView::SetDrawState(CadDrawState state)
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_drawState = state;
    m_bDrawing = (state != STATE_IDLE);

    // Only deselect for drawing commands, not modify/select/view operations
    if (state >= STATE_DRAW_LINE_P1 && state <= STATE_DRAW_TEXT_POS)
        pDoc->DeselectAll();

    TRACE(L"[DEBUG] SetDrawState: state=%d, m_bDrawing=%d, tempPts=%d\n", (int)state, m_bDrawing, (int)m_tempPts.size());

    switch (state) {
    case STATE_DRAW_LINE_P1:         pDoc->m_strCommandPrompt = L"LINE Specify first point: "; break;
    case STATE_DRAW_LINE_P2:         pDoc->m_strCommandPrompt = L"LINE Specify next point: "; break;
    case STATE_DRAW_CIRCLE_CENTER:   pDoc->m_strCommandPrompt = L"CIRCLE Specify center: "; break;
    case STATE_DRAW_CIRCLE_RADIUS:   pDoc->m_strCommandPrompt = L"CIRCLE Specify radius: "; break;
    case STATE_DRAW_ARC_P1:          pDoc->m_strCommandPrompt = L"ARC Specify start point: "; break;
    case STATE_DRAW_ARC_P2:          pDoc->m_strCommandPrompt = L"ARC Specify center point: "; break;
    case STATE_DRAW_ARC_P3:          pDoc->m_strCommandPrompt = L"ARC Specify end point: "; break;
    case STATE_DRAW_ARC_PREVIEW:     pDoc->m_strCommandPrompt = L"ARC Press ENTER to confirm, Ctrl to flip half: "; break;
    case STATE_DRAW_RECT_P1:         pDoc->m_strCommandPrompt = L"RECTANG Specify first corner: "; break;
    case STATE_DRAW_RECT_P2:         pDoc->m_strCommandPrompt = L"RECTANG Specify other corner: "; break;
    case STATE_DRAW_POLYGON_CENTER:  pDoc->m_strCommandPrompt = L"POLYGON Specify center: "; break;
    case STATE_DRAW_POLYGON_RADIUS:  pDoc->m_strCommandPrompt = L"POLYGON Specify radius: "; break;
    case STATE_DRAW_ELLIPSE_CENTER:  pDoc->m_strCommandPrompt = L"ELLIPSE Specify center: "; break;
    case STATE_DRAW_ELLIPSE_RADIUS:  pDoc->m_strCommandPrompt = L"ELLIPSE Specify axis endpoint: "; break;
    case STATE_DRAW_POLYLINE_POINT:  pDoc->m_strCommandPrompt = L"PLINE Specify next point (ENTER to finish): "; break;
    case STATE_DRAW_TEXT_POS:        pDoc->m_strCommandPrompt = L"TEXT Specify position: "; break;
    case STATE_MOVE_SELECT:          pDoc->m_strCommandPrompt = L"MOVE Select objects: "; break;
    case STATE_MOVE_BASE:            pDoc->m_strCommandPrompt = L"MOVE Specify base point: "; break;
    case STATE_MOVE_DEST:            pDoc->m_strCommandPrompt = L"MOVE Specify destination: "; break;
    case STATE_COPY_SELECT:          pDoc->m_strCommandPrompt = L"COPY Select objects: "; break;
    case STATE_COPY_BASE:            pDoc->m_strCommandPrompt = L"COPY Specify base point: "; break;
    case STATE_COPY_DEST:            pDoc->m_strCommandPrompt = L"COPY Specify destination: "; break;
    case STATE_ROTATE_SELECT:        pDoc->m_strCommandPrompt = L"ROTATE Select objects: "; break;
    case STATE_ROTATE_CENTER:        pDoc->m_strCommandPrompt = L"ROTATE Specify base point: "; break;
    case STATE_ROTATE_ANGLE:         pDoc->m_strCommandPrompt = L"ROTATE Specify rotation angle or second point: "; break;
    case STATE_SCALE_SELECT:         pDoc->m_strCommandPrompt = L"SCALE Select objects: "; break;
    case STATE_SCALE_BASE:           pDoc->m_strCommandPrompt = L"SCALE Specify base point: "; break;
    case STATE_SCALE_FACTOR:         pDoc->m_strCommandPrompt = L"SCALE Specify scale factor or second point: "; break;
    case STATE_MIRROR_SELECT:        pDoc->m_strCommandPrompt = L"MIRROR Select objects: "; break;
    case STATE_MIRROR_P1:            pDoc->m_strCommandPrompt = L"MIRROR Specify first point of mirror line: "; break;
    case STATE_MIRROR_P2:            pDoc->m_strCommandPrompt = L"MIRROR Specify second point of mirror line: "; break;
    case STATE_OFFSET_SELECT:        pDoc->m_strCommandPrompt = L"OFFSET Select entity to offset: "; break;
    case STATE_OFFSET_DIST:          pDoc->m_strCommandPrompt = L"OFFSET Specify offset distance (click point on side): "; break;
    case STATE_ZOOM_WINDOW_P1:       pDoc->m_strCommandPrompt = L"ZOOM Window: Specify first corner: "; break;
    case STATE_ZOOM_WINDOW_P2:       pDoc->m_strCommandPrompt = L"ZOOM Window: Specify opposite corner: "; break;
    case STATE_IDLE:
    default:                         pDoc->m_strCommandPrompt = L"Command: "; m_bDrawing = false; break;
    }
    
    // Update command line edit (only on state change, not every mouse move)
    CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
    if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
        pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
        int nLen = pDoc->m_strCommandPrompt.GetLength();
        pFrame->m_wndCmdLine.SetSel(nLen, nLen);
    }
    
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::CompleteDrawCommand()
{
    TRACE(L"[DEBUG] CompleteDrawCommand called\n");
    m_tempPts.clear();
    SetDrawState(STATE_IDLE);
}

// ============================================================
// LButtonDown - Interaction state machine core
// ============================================================
void CLargeHWView::OnLButtonDown(UINT nFlags, CPoint point)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    CPoint world = ScreenToWorld(point);

    // Apply snap (grid + object)
    CPoint worldSnapped = SnapToGrid(world);
    if (pDoc->m_bObjectSnap) {
        worldSnapped = SnapToObjects(worldSnapped);
    }

    // Apply ortho if in a state that requires it
    CadDrawState state = pDoc->m_drawState;
    TRACE(L"[DEBUG] OnLButtonDown: screen=(%d,%d), world=(%d,%d), state=%d, tempPts=%d, selected=%d\n",
          point.x, point.y, world.x, world.y, (int)state, (int)m_tempPts.size(), pDoc->GetSelectedCount());
    if (pDoc->m_bOrthoMode && !m_tempPts.empty() &&
        (state == STATE_DRAW_LINE_P2 || state == STATE_MOVE_DEST || state == STATE_COPY_DEST ||
         state == STATE_ROTATE_ANGLE || state == STATE_SCALE_FACTOR || state == STATE_MIRROR_P2 ||
         state == STATE_OFFSET_DIST || state == STATE_DRAW_RECT_P2)) {
        worldSnapped = ApplyOrtho(worldSnapped, m_tempPts.back());
    }

    world = worldSnapped;

    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine &&
        state != STATE_DRAW_TEXT_POS && ShouldRecordPointForState(state)) {
        RecordScriptInput(FormatScriptPoint(world));
    }

    // Check grip hit first (only in IDLE state)
    if (state == STATE_IDLE) {
        CEntity* hitEntity = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hitEntity && hitEntity->m_bSelected) {
            int gripIdx = hitEntity->HitTestGrip(point, pDoc->m_dScale, pDoc->m_ptOffset);
            if (gripIdx >= 0) {
                pDoc->RecordGripUndo(hitEntity);
                m_nGripIndex = gripIdx;
                m_pGripEntity = hitEntity;
                m_bDragging = true;
                m_ptDragStart = point;
                return;
            }
        }
    }

    switch (state) {
    case STATE_IDLE: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hit) {
            if (!(nFlags & MK_CONTROL))
                pDoc->DeselectAll();
            hit->m_bSelected = true;
        } else {
            // Ctrl+click on empty space starts zoom window drag
            if (nFlags & MK_CONTROL) {
                pDoc->m_drawState = STATE_ZOOM_WINDOW_P1;
                m_bDrawing = true;
                m_ptDragStart = point;
                m_bDragging = true;
                pDoc->m_strCommandPrompt = L"ZOOM Window: Specify first corner: ";
                UpdateStatusBar();
            } else {
                pDoc->DeselectAll();
                pDoc->m_drawState = STATE_WINDOW_SELECT;
                m_ptDragStart = point;
                m_bDragging = true;
            }
        }
        Invalidate(FALSE);
        break;
    }

    case STATE_DRAW_LINE_P1:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_LINE_P2);
        break;

    case STATE_DRAW_LINE_P2: {
        m_tempPts.push_back(world);
        CLineEntity* pLine = new CLineEntity(m_tempPts[0], m_tempPts[1]);
        pDoc->AddEntity(pLine);
        CompleteDrawCommand();
        break;
    }

    case STATE_DRAW_CIRCLE_CENTER:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_CIRCLE_RADIUS);
        break;

    case STATE_DRAW_CIRCLE_RADIUS: {
        int r = (int)Distance(m_tempPts[0], world);
        CCircleEntity* pCircle = new CCircleEntity(m_tempPts[0], r);
        pDoc->AddEntity(pCircle);
        CompleteDrawCommand();
        break;
    }

    case STATE_DRAW_ARC_P1:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_ARC_P2);
        break;

    case STATE_DRAW_ARC_P2:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_ARC_P3);
        break;

    case STATE_DRAW_ARC_P3: {
        m_tempPts.push_back(world);
        m_bArcAltHalf = false;
        SetDrawState(STATE_DRAW_ARC_PREVIEW);
        break;
    }

    case STATE_DRAW_RECT_P1:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_RECT_P2);
        break;

    case STATE_DRAW_RECT_P2: {
        m_tempPts.push_back(world);
        CRectangleEntity* pRect = new CRectangleEntity(m_tempPts[0], m_tempPts[1]);
        pDoc->AddEntity(pRect);
        CompleteDrawCommand();
        break;
    }

    case STATE_DRAW_POLYGON_CENTER:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_POLYGON_RADIUS);
        break;

    case STATE_DRAW_POLYGON_RADIUS: {
        int r = (int)Distance(m_tempPts[0], world);
        CPolygonEntity* pPoly = new CPolygonEntity(m_tempPts[0], r, m_nPolygonSides);
        pDoc->AddEntity(pPoly);
        CompleteDrawCommand();
        break;
    }

    case STATE_DRAW_ELLIPSE_CENTER:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_ELLIPSE_RADIUS);
        break;

    case STATE_DRAW_ELLIPSE_RADIUS: {
        int rx = abs(world.x - m_tempPts[0].x);
        int ry = abs(world.y - m_tempPts[0].y);
        CEllipseEntity* pEllipse = new CEllipseEntity(m_tempPts[0], rx, ry);
        pDoc->AddEntity(pEllipse);
        CompleteDrawCommand();
        break;
    }

    case STATE_DRAW_POLYLINE_POINT:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_POLYLINE_POINT);
        break;

    case STATE_DRAW_TEXT_POS: {
        CTextInputDlg dlg(this);
        if (dlg.DoModal() == IDOK) {
            CTextEntity* pText = new CTextEntity(world, dlg.m_strText, dlg.m_nHeight);
            pDoc->AddEntity(pText);
            if (m_bScriptRecording && !m_bRunningScript) {
                CString strTextCommand;
                strTextCommand.Format(L"TEXT %d,%d %d \"%s\"",
                                      world.x, world.y, dlg.m_nHeight,
                                      (LPCTSTR)EscapeScriptText(dlg.m_strText));
                RecordScriptInput(strTextCommand);
            }
        }
        CompleteDrawCommand();
        break;
    }

    case STATE_MOVE_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_MOVE_SELECT: hit=%p, world=(%d,%d)\n", hit, world.x, world.y);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            m_tempPts.push_back(world);
            SetDrawState(STATE_MOVE_BASE);
        }
        break;
    }

    case STATE_MOVE_BASE:
        TRACE(L"[DEBUG] STATE_MOVE_BASE: world=(%d,%d), tempPts=%d\n", world.x, world.y, (int)m_tempPts.size());
        m_tempPts.push_back(world);
        SetDrawState(STATE_MOVE_DEST);
        break;

    case STATE_MOVE_DEST: {
        pDoc->RecordModifyUndo();
        double dx = world.x - m_tempPts[1].x;
        double dy = world.y - m_tempPts[1].y;
        int nCount = pDoc->GetSelectedCount();
        TRACE(L"[DEBUG] STATE_MOVE_DEST: world=(%d,%d), base=(%d,%d), dx=%.0f, dy=%.0f, nSel=%d, tempPts=%d\n",
              world.x, world.y, m_tempPts[1].x, m_tempPts[1].y, dx, dy, nCount, (int)m_tempPts.size());
        pDoc->MoveSelected(dx, dy);
        pDoc->DeselectAll();
        CompleteDrawCommand();
        pDoc->m_strCommandPrompt.Format(L"Moved %d entities (dx=%d, dy=%d)", nCount, (int)dx, (int)dy);
        UpdateStatusBar();
        break;
    }

    case STATE_COPY_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_COPY_SELECT: hit=%p, world=(%d,%d)\n", hit, world.x, world.y);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            m_tempPts.push_back(world);
            SetDrawState(STATE_COPY_BASE);
        }
        break;
    }

    case STATE_COPY_BASE:
        TRACE(L"[DEBUG] STATE_COPY_BASE: world=(%d,%d), tempPts=%d\n", world.x, world.y, (int)m_tempPts.size());
        m_tempPts.push_back(world);
        SetDrawState(STATE_COPY_DEST);
        break;

    case STATE_COPY_DEST: {
        double dx = world.x - m_tempPts[1].x;
        double dy = world.y - m_tempPts[1].y;
        TRACE(L"[DEBUG] STATE_COPY_DEST: world=(%d,%d), base=(%d,%d), dx=%.0f, dy=%.0f, tempPts=%d\n",
              world.x, world.y, m_tempPts[1].x, m_tempPts[1].y, dx, dy, (int)m_tempPts.size());
        auto selectedEnts = pDoc->GetSelectedEntities();
        int nCount = (int)selectedEnts.size();
        std::vector<int> newIDs;
        for (auto* pEnt : selectedEnts) {
            CEntity* pCopy = pEnt->Clone();
            pCopy->Move(dx, dy);
            pDoc->AddEntity(pCopy, false);
            newIDs.push_back(pCopy->m_nID);
        }
        pDoc->RecordCreateUndo(newIDs);
        pDoc->DeselectAll();
        CompleteDrawCommand();
        pDoc->m_strCommandPrompt.Format(L"Copied %d entities (dx=%d, dy=%d)", nCount, (int)dx, (int)dy);
        UpdateStatusBar();
        break;
    }

    case STATE_ROTATE_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_ROTATE_SELECT: hit=%p\n", hit);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            SetDrawState(STATE_ROTATE_CENTER);
        }
        break;
    }

    case STATE_ROTATE_CENTER:
        TRACE(L"[DEBUG] STATE_ROTATE_CENTER: world=(%d,%d)\n", world.x, world.y);
        m_tempPts.push_back(world);
        SetDrawState(STATE_ROTATE_ANGLE);
        break;

    case STATE_ROTATE_ANGLE: {
        MessageBeep(MB_ICONASTERISK);
        pDoc->RecordModifyUndo();
        CPoint base = m_tempPts[0];
        double angle = atan2((double)(world.y - base.y), (double)(world.x - base.x));
        int nCount = pDoc->GetSelectedCount();
        TRACE(L"[DEBUG] STATE_ROTATE_ANGLE: world=(%d,%d), base=(%d,%d), angle=%.3f rad (%.1f deg), nSel=%d\n",
              world.x, world.y, base.x, base.y, angle, angle * 180.0 / M_PI, nCount);
        if (nCount == 0) {
            pDoc->m_strCommandPrompt = L"ROTATE: No entities selected! Select objects first.";
        } else {
            pDoc->RotateSelected(base, angle);
            pDoc->m_strCommandPrompt.Format(L"Rotated %d entities (angle=%.1f deg)", nCount, angle * 180.0 / M_PI);
        }
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        break;
    }

    case STATE_SCALE_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_SCALE_SELECT: hit=%p\n", hit);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            SetDrawState(STATE_SCALE_BASE);
        }
        break;
    }

    case STATE_SCALE_BASE:
        TRACE(L"[DEBUG] STATE_SCALE_BASE: world=(%d,%d)\n", world.x, world.y);
        m_tempPts.push_back(world);
        SetDrawState(STATE_SCALE_FACTOR);
        break;

    case STATE_SCALE_FACTOR: {
        MessageBeep(MB_ICONASTERISK);
        pDoc->RecordModifyUndo();
        CPoint base = m_tempPts[0];
        double dist = Distance(base, world);
        if (dist < 1.0) dist = 1.0;
        double factor = dist / 100.0;
        int nCount = pDoc->GetSelectedCount();
        TRACE(L"[DEBUG] STATE_SCALE_FACTOR: world=(%d,%d), base=(%d,%d), dist=%.0f, factor=%.3f, nSel=%d\n",
              world.x, world.y, base.x, base.y, dist, factor, nCount);
        if (nCount == 0) {
            pDoc->m_strCommandPrompt = L"SCALE: No entities selected!";
        } else {
            pDoc->ScaleSelected(base, factor);
            pDoc->m_strCommandPrompt.Format(L"Scaled %d entities (factor=%.2f)", nCount, factor);
        }
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        break;
    }

    case STATE_MIRROR_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_MIRROR_SELECT: hit=%p\n", hit);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            SetDrawState(STATE_MIRROR_P1);
        }
        break;
    }

    case STATE_MIRROR_P1:
        TRACE(L"[DEBUG] STATE_MIRROR_P1: world=(%d,%d)\n", world.x, world.y);
        m_tempPts.push_back(world);
        SetDrawState(STATE_MIRROR_P2);
        break;

    case STATE_MIRROR_P2: {
        CPoint p1 = m_tempPts[0];
        CPoint p2 = world;
        double distP1P2 = Distance(p1, p2);
        int nCount = pDoc->GetSelectedCount();
        TRACE(L"[DEBUG] STATE_MIRROR_P2: p1=(%d,%d), p2=(%d,%d), dist=%.0f, nSel=%d\n",
              p1.x, p1.y, p2.x, p2.y, distP1P2, nCount);
        if (distP1P2 > 0) {
            pDoc->RecordModifyUndo();
            if (nCount == 0) {
                pDoc->m_strCommandPrompt = L"MIRROR: No entities selected!";
            } else {
                pDoc->MirrorSelected(p1, p2);
                pDoc->m_strCommandPrompt.Format(L"Mirrored %d entities", nCount);
            }
        } else {
            pDoc->m_strCommandPrompt = L"Mirror: line too short, cancelled";
        }
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        break;
    }

    case STATE_OFFSET_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_OFFSET_SELECT: hit=%p\n", hit);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            m_tempPts.push_back(world);
            SetDrawState(STATE_OFFSET_DIST);
        }
        break;
    }

    case STATE_OFFSET_DIST: {
        CPoint ref = m_tempPts[0];
        double dx = world.x - ref.x;
        double dy = world.y - ref.y;
        TRACE(L"[DEBUG] STATE_OFFSET_DIST: world=(%d,%d), ref=(%d,%d), dx=%.0f, dy=%.0f\n",
              world.x, world.y, ref.x, ref.y, dx, dy);
        auto selectedEnts = pDoc->GetSelectedEntities();
        int nCount = (int)selectedEnts.size();
        std::vector<int> newIDs;
        for (auto* pEnt : selectedEnts) {
            CEntity* pCopy = pEnt->Clone();
            pCopy->Move(dx, dy);
            pDoc->AddEntity(pCopy, false);
            newIDs.push_back(pCopy->m_nID);
        }
        pDoc->RecordCreateUndo(newIDs);
        pDoc->DeselectAll();
        CompleteDrawCommand();
        pDoc->m_strCommandPrompt.Format(L"Offset %d entities (dx=%d, dy=%d)", nCount, (int)dx, (int)dy);
        UpdateStatusBar();
        break;
    }

    case STATE_ZOOM_WINDOW_P1:
        m_ptDragStart = point;
        SetDrawState(STATE_ZOOM_WINDOW_P2);
        break;

    case STATE_ZOOM_WINDOW_P2: {
        CRect rc(m_ptDragStart, point);
        rc.NormalizeRect();
        if (rc.Width() > 5 && rc.Height() > 5) {
            CPoint topLeft = ScreenToWorld(rc.TopLeft());
            CPoint bottomRight = ScreenToWorld(rc.BottomRight());
            CRect rcWorld(topLeft, bottomRight);
            rcWorld.NormalizeRect();
            rcWorld.InflateRect(10, 10);

            CRect rcClient;
            GetClientRect(&rcClient);

            if (rcWorld.Width() > 0 && rcWorld.Height() > 0) {
                double sx = (double)rcClient.Width() / rcWorld.Width();
                double sy = (double)rcClient.Height() / rcWorld.Height();
                pDoc->m_dScale = min(sx, sy);
                pDoc->m_ptOffset = CPoint(
                    (int)(-rcWorld.left * pDoc->m_dScale),
                    (int)(-rcWorld.top * pDoc->m_dScale)
                );
            }
        }
        CompleteDrawCommand();
        UpdateStatusBar();
        Invalidate(FALSE);
        break;
    }

    default:
        break;
    }

    m_ptCurrent = point;
    UpdateStatusBar();
    Invalidate(FALSE);

    CView::OnLButtonDown(nFlags, point);
}

// ============================================================
// LButtonUp
// ============================================================
void CLargeHWView::OnLButtonUp(UINT nFlags, CPoint point)
{
    CLargeHWDoc* pDoc = GetDocument();

    if (pDoc->m_drawState == STATE_WINDOW_SELECT && m_bDragging) {
        CRect rc(m_ptDragStart, point);
        rc.NormalizeRect();
        if (rc.Width() > 5 && rc.Height() > 5) {
            pDoc->SelectByWindow(rc, pDoc->m_dScale, pDoc->m_ptOffset);
        } else {
            pDoc->SelectByPoint(point, pDoc->m_dScale, pDoc->m_ptOffset);
        }
        pDoc->m_drawState = STATE_IDLE;
        m_bDragging = false;
        m_bDrawing = false;
        UpdateStatusBar();
        Invalidate(FALSE);
    }

    if (pDoc->m_drawState == STATE_ZOOM_WINDOW_P1 && m_bDragging) {
        CRect rc(m_ptDragStart, point);
        rc.NormalizeRect();
        if (rc.Width() > 5 && rc.Height() > 5) {
            CPoint topLeft = ScreenToWorld(rc.TopLeft());
            CPoint bottomRight = ScreenToWorld(rc.BottomRight());
            CRect rcWorld(topLeft, bottomRight);
            rcWorld.NormalizeRect();
            rcWorld.InflateRect(10, 10);

            CRect rcClient;
            GetClientRect(&rcClient);

            if (rcWorld.Width() > 0 && rcWorld.Height() > 0) {
                double sx = (double)rcClient.Width() / rcWorld.Width();
                double sy = (double)rcClient.Height() / rcWorld.Height();
                pDoc->m_dScale = min(sx, sy);
                pDoc->m_ptOffset = CPoint(
                    (int)(-rcWorld.left * pDoc->m_dScale),
                    (int)(-rcWorld.top * pDoc->m_dScale)
                );
                pDoc->m_strCommandPrompt.Format(L"Zoom Window: scale=%.2f", pDoc->m_dScale);
            }
        } else {
            pDoc->m_strCommandPrompt = L"Zoom Window: area too small, cancelled";
        }
        pDoc->m_drawState = STATE_IDLE;
        m_bDragging = false;
        m_bDrawing = false;
        UpdateStatusBar();
        Invalidate(FALSE);
    }

    if (m_bDragging && m_nGripIndex >= 0 && m_pGripEntity) {
        CPoint world = ScreenToWorld(point);
        m_pGripEntity->SetGrip(m_nGripIndex, world);
        m_bDragging = false;
        m_nGripIndex = -1;
        m_pGripEntity = nullptr;
        pDoc->SetModified(true);
        Invalidate(FALSE);
    }

    CView::OnLButtonUp(nFlags, point);
}

// ============================================================
// MouseMove - Update coordinates + preview
// ============================================================
void CLargeHWView::OnMouseMove(UINT nFlags, CPoint point)
{
    CLargeHWDoc* pDoc = GetDocument();

    // Pan with middle button
    if (m_bPanning) {
        CPoint delta(point.x - m_ptPanStart.x, point.y - m_ptPanStart.y);
        pDoc->m_ptOffset = CPoint(m_ptPanOffsetStart.x + delta.x, m_ptPanOffsetStart.y + delta.y);
        m_ptCurrent = point;
        UpdateStatusBar();
        Invalidate(FALSE);
        return;
    }

    // Always track raw mouse for smooth crosshair
    m_ptCurrent = point;

    // Compute snapped world point
    CPoint world = ScreenToWorld(point);
    world = SnapToGrid(world);
    CPoint worldBeforeSnap = world;
    if (pDoc->m_bObjectSnap) {
        world = SnapToObjects(world);
    }
    m_bSnapActive = (worldBeforeSnap.x != world.x || worldBeforeSnap.y != world.y);
    if (m_bSnapActive) {
        m_ptSnapped = WorldToScreen(world);
    }

    // Grip drag with ortho
    if (m_bDragging && m_nGripIndex >= 0 && m_pGripEntity) {
        CPoint refGrip = m_pGripEntity->GetGrip(0);
        world = ApplyOrtho(world, refGrip);
        m_pGripEntity->SetGrip(m_nGripIndex, world);
        pDoc->SetModified(true);
        UpdateStatusBar();
        Invalidate(FALSE);
        return;
    }

    // Apply ortho for drawing/modify commands
    CadDrawState state = pDoc->m_drawState;
    if ((state == STATE_DRAW_LINE_P2 || state == STATE_MOVE_DEST || state == STATE_COPY_DEST ||
         state == STATE_ROTATE_ANGLE || state == STATE_SCALE_FACTOR || state == STATE_MIRROR_P2 ||
         state == STATE_OFFSET_DIST) && !m_tempPts.empty()) {
        world = ApplyOrtho(world, m_tempPts.back());
        m_ptSnapped = WorldToScreen(world);
        m_bSnapActive = true;
    }

    UpdateStatusBar();

    // Always invalidate to keep crosshair tracking and snap marker live
    Invalidate(FALSE);

    CView::OnMouseMove(nFlags, point);
}

// ============================================================
// RButtonDown - Context menu / End polyline
// ============================================================
void CLargeHWView::OnRButtonDown(UINT nFlags, CPoint point)
{
    CLargeHWDoc* pDoc = GetDocument();

    CView::OnRButtonDown(nFlags, point);
}

// ============================================================
// MouseWheel - Zoom
// ============================================================
BOOL CLargeHWView::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
    CLargeHWDoc* pDoc = GetDocument();

    CPoint worldBefore = ScreenToWorld(m_ptCurrent);

    double zoomFactor = (zDelta > 0) ? 1.1 : 1.0 / 1.1;
    double newScale = pDoc->m_dScale * zoomFactor;
    if (newScale < 0.05) newScale = 0.05;
    if (newScale > 20.0) newScale = 20.0;

    pDoc->m_dScale = newScale;

    CPoint worldAfter = ScreenToWorld(m_ptCurrent);
    pDoc->m_ptOffset.x += (int)((worldAfter.x - worldBefore.x) * pDoc->m_dScale);
    pDoc->m_ptOffset.y += (int)((worldAfter.y - worldBefore.y) * pDoc->m_dScale);

    UpdateStatusBar();
    Invalidate(FALSE);

    return TRUE;
}

// ============================================================
// MButton Down/Up - Pan with middle mouse
// ============================================================
void CLargeHWView::OnMButtonDown(UINT nFlags, CPoint point)
{
    CLargeHWDoc* pDoc = GetDocument();
    m_bPanning = true;
    m_ptPanStart = point;
    m_ptPanOffsetStart = pDoc->m_ptOffset;
    SetCapture();
    CView::OnMButtonDown(nFlags, point);
}

void CLargeHWView::OnMButtonUp(UINT nFlags, CPoint point)
{
    m_bPanning = false;
    ReleaseCapture();
    UpdateStatusBar();
    Invalidate(FALSE);
    CView::OnMButtonUp(nFlags, point);
}

// ============================================================
// Keyboard
// ============================================================
void CLargeHWView::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
    CLargeHWDoc* pDoc = GetDocument();

    switch (nChar) {
    case VK_ESCAPE:
        OnCancelCommand();
        break;

    case 'C':
        if (pDoc->m_drawState == STATE_DRAW_POLYLINE_POINT && m_tempPts.size() >= 2) {
            if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
                RecordScriptInput(L"C");
            m_bPolylineClose = !m_bPolylineClose;
            pDoc->m_strCommandPrompt.Format(
                L"PLINE Specify next point (ENTER to finish) [CLOSE=%s]: ",
                m_bPolylineClose ? L"ON" : L"OFF");
            UpdateStatusBar();
            Invalidate(FALSE);
            return;
        }
        break;

    case VK_RETURN:
        if (pDoc->m_drawState == STATE_DRAW_POLYLINE_POINT && m_tempPts.size() >= 2) {
            if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
                RecordScriptInput(L"");
            CPolylineEntity* pPline = new CPolylineEntity(m_tempPts);
            pPline->m_bClosed = m_bPolylineClose;
            pDoc->AddEntity(pPline);
            m_bPolylineClose = false;
            CompleteDrawCommand();
            Invalidate(FALSE);
        }
        if (pDoc->m_drawState == STATE_DRAW_ARC_PREVIEW && m_tempPts.size() >= 3) {
            if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
                RecordScriptInput(L"");
            // Build arc: tempPts[0]=start, [1]=center, [2]=end
            CPoint ptStart  = m_tempPts[0];
            CPoint ptCenter = m_tempPts[1];
            CPoint ptEnd    = m_tempPts[2];
            int r = (int)Distance(ptCenter, ptStart);
            double angS = atan2(ptStart.y - ptCenter.y, ptStart.x - ptCenter.x);
            double angE = atan2(ptEnd.y - ptCenter.y, ptEnd.x - ptCenter.x);
            double sweep = angE - angS;
            if (sweep < 0) sweep += 2.0 * M_PI;

            CArcEntity* pArc = new CArcEntity();
            if (!m_bArcAltHalf) {
                pArc->SetArcByCenter(ptCenter, r, angS, angE);
            } else {
                pArc->SetArcByCenter(ptCenter, r, angE, angS);
            }
            pDoc->AddEntity(pArc);
            CompleteDrawCommand();
            Invalidate(FALSE);
        }
        break;

    case VK_DELETE:
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
            RecordScriptInput(L"ERASE");
        pDoc->DeleteSelected();
        Invalidate(FALSE);
        break;

    case 'Z':
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            pDoc->Undo();
            UpdateStatusBar();
            Invalidate(FALSE);
        } else if (pDoc->m_drawState == STATE_IDLE) {
            OnViewZoomWindow();
        }
        break;

    case 'Y':
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            pDoc->Redo();
            UpdateStatusBar();
            Invalidate(FALSE);
        }
        break;

    case 'A':
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            for (auto* p : pDoc->GetEntities())
                const_cast<CEntity*>(p)->m_bSelected = true;
            Invalidate(FALSE);
        }
        break;

    case VK_F7:
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
            RecordScriptInput(L"GRID");
        pDoc->m_bShowGrid = !pDoc->m_bShowGrid;
        UpdateStatusBar();
        Invalidate(FALSE);
        break;

    case VK_F9:
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
            RecordScriptInput(L"SNAP");
        pDoc->m_bSnapToGrid = !pDoc->m_bSnapToGrid;
        UpdateStatusBar();
        Invalidate(FALSE);
        break;

    case VK_F8:
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
            RecordScriptInput(L"ORTHO");
        pDoc->m_bOrthoMode = !pDoc->m_bOrthoMode;
        UpdateStatusBar();
        Invalidate(FALSE);
        break;

    case VK_F3:
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
            RecordScriptInput(L"OSNAP");
        pDoc->m_bObjectSnap = !pDoc->m_bObjectSnap;
        UpdateStatusBar();
        Invalidate(FALSE);
        break;

    case VK_CONTROL:
        if (pDoc->m_drawState == STATE_DRAW_ARC_PREVIEW && !(nFlags & 0x4000)) {
            // Toggle arc half (only on keydown, not repeat)
            m_bArcAltHalf = !m_bArcAltHalf;
            Invalidate(FALSE);
            return;
        }
        break;
    }

    CView::OnKeyDown(nChar, nRepCnt, nFlags);
}

// ============================================================
// Cursor - Show crosshair on client area
// ============================================================
BOOL CLargeHWView::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
    if (nHitTest == HTCLIENT) {
        HCURSOR hCross = AfxGetApp()->LoadStandardCursor(IDC_CROSS);
        if (hCross) {
            SetCursor(hCross);
            return TRUE;
        }
    }
    return CView::OnSetCursor(pWnd, nHitTest, message);
}

// ============================================================
// OnSize
// ============================================================
void CLargeHWView::OnSize(UINT nType, int cx, int cy)
{
    CView::OnSize(nType, cx, cy);
    Invalidate(FALSE);
}

// ============================================================
// Context menu
// ============================================================
void CLargeHWView::OnContextMenu(CWnd* pWnd, CPoint pt)
{
    CMenu menu;
    menu.CreatePopupMenu();

    menu.AppendMenu(MF_STRING, ID_CONTEXT_ENTER,  L"Enter");
    menu.AppendMenu(MF_STRING, ID_CONTEXT_CANCEL, L"Cancel (Esc)");
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, ID_CONTEXT_REPEAT, L"Repeat last command");
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, ID_VIEW_ZOOM_WINDOW,  L"Zoom Window");
    menu.AppendMenu(MF_STRING, ID_VIEW_ZOOM_EXTENTS, L"Zoom Extents");

    CLargeHWDoc* pDoc = GetDocument();
    int nSel = pDoc->GetSelectedCount();
    if (nSel > 0) {
        menu.AppendMenu(MF_SEPARATOR);
        CString strSel;
        strSel.Format(L"Selected %d objects", nSel);
        menu.AppendMenu(MF_STRING | MF_GRAYED, 0, strSel);
        menu.AppendMenu(MF_STRING, ID_MODIFY_DELETE, L"Delete");
        menu.AppendMenu(MF_STRING, ID_MODIFY_MOVE,   L"Move");
        menu.AppendMenu(MF_STRING, ID_MODIFY_COPY,   L"Copy");
        menu.AppendMenu(MF_STRING, ID_MODIFY_ROTATE, L"Rotate");
        menu.AppendMenu(MF_STRING, ID_MODIFY_SCALE,  L"Scale");
        menu.AppendMenu(MF_STRING, ID_MODIFY_MIRROR, L"Mirror");
    }

    if (pt.x == -1 && pt.y == -1) {
        CRect rc; GetClientRect(&rc);
        pt = rc.CenterPoint();
        ClientToScreen(&pt);
    }

    menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, this);
}

// ============================================================
// Draw command handlers
// ============================================================
void CLargeHWView::OnDrawLine()       { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINE"); m_tempPts.clear(); m_nLastCommandID = ID_DRAW_LINE; SetDrawState(STATE_DRAW_LINE_P1); }
void CLargeHWView::OnDrawCircle()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"CIRCLE"); m_tempPts.clear(); m_nLastCommandID = ID_DRAW_CIRCLE; SetDrawState(STATE_DRAW_CIRCLE_CENTER); }
void CLargeHWView::OnDrawArc()        { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"ARC"); m_tempPts.clear(); m_nLastCommandID = ID_DRAW_ARC; SetDrawState(STATE_DRAW_ARC_P1); }
void CLargeHWView::OnDrawRectangle()  { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"RECTANGLE"); m_tempPts.clear(); m_nLastCommandID = ID_DRAW_RECTANGLE; SetDrawState(STATE_DRAW_RECT_P1); }
void CLargeHWView::OnDrawEllipse()    { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"ELLIPSE"); m_tempPts.clear(); m_nLastCommandID = ID_DRAW_ELLIPSE; SetDrawState(STATE_DRAW_ELLIPSE_CENTER); }
void CLargeHWView::OnDrawPolyline()   { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"PLINE"); m_tempPts.clear(); m_bPolylineClose = false; m_nLastCommandID = ID_DRAW_POLYLINE; SetDrawState(STATE_DRAW_POLYLINE_POINT); }
void CLargeHWView::OnDrawText()       { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_TEXT; SetDrawState(STATE_DRAW_TEXT_POS); }
void CLargeHWView::OnDrawPolygon()    { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"POLYGON"); m_tempPts.clear(); m_nPolygonSides = 6; m_nLastCommandID = ID_DRAW_POLYGON; SetDrawState(STATE_DRAW_POLYGON_CENTER); }

// ============================================================
// Modify command handlers
// ============================================================
void CLargeHWView::OnModifyMove()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"MOVE");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    m_nLastCommandID = ID_MODIFY_MOVE;
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyMove called, selected=%d, entCount=%d\n", nSel, (int)pDoc->GetEntities().size());
    if (nSel > 0) {
        m_tempPts.push_back(CPoint(0, 0));
        SetDrawState(STATE_MOVE_BASE);
        TRACE(L"[DEBUG] OnModifyMove: entities pre-selected, state=STATE_MOVE_BASE\n");
    } else {
        SetDrawState(STATE_MOVE_SELECT);
        TRACE(L"[DEBUG] OnModifyMove: no selection, state=STATE_MOVE_SELECT\n");
    }
}

void CLargeHWView::OnModifyCopy()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"COPY");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    m_nLastCommandID = ID_MODIFY_COPY;
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyCopy called, selected=%d, entCount=%d\n", nSel, (int)pDoc->GetEntities().size());
    if (nSel > 0) {
        m_tempPts.push_back(CPoint(0, 0));
        SetDrawState(STATE_COPY_BASE);
        TRACE(L"[DEBUG] OnModifyCopy: entities pre-selected, state=STATE_COPY_BASE\n");
    } else {
        SetDrawState(STATE_COPY_SELECT);
        TRACE(L"[DEBUG] OnModifyCopy: no selection, state=STATE_COPY_SELECT\n");
    }
}

void CLargeHWView::OnModifyDelete()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ERASE");
    CLargeHWDoc* pDoc = GetDocument();
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyDelete called, selected=%d\n", nSel);
    pDoc->DeleteSelected();
    Invalidate(FALSE);
}

void CLargeHWView::OnModifyRotate()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ROTATE");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    m_nLastCommandID = ID_MODIFY_ROTATE;
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyRotate called, selected=%d, entCount=%d\n", nSel, (int)pDoc->GetEntities().size());
    if (nSel > 0) {
        SetDrawState(STATE_ROTATE_CENTER);
        TRACE(L"[DEBUG] OnModifyRotate: entities pre-selected, state=STATE_ROTATE_CENTER\n");
    } else {
        SetDrawState(STATE_ROTATE_SELECT);
        TRACE(L"[DEBUG] OnModifyRotate: no selection, state=STATE_ROTATE_SELECT\n");
    }
}

void CLargeHWView::OnModifyScale()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"SCALE");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    m_nLastCommandID = ID_MODIFY_SCALE;
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyScale called, selected=%d, entCount=%d\n", nSel, (int)pDoc->GetEntities().size());
    if (nSel > 0) {
        SetDrawState(STATE_SCALE_BASE);
        TRACE(L"[DEBUG] OnModifyScale: entities pre-selected, state=STATE_SCALE_BASE\n");
    } else {
        SetDrawState(STATE_SCALE_SELECT);
        TRACE(L"[DEBUG] OnModifyScale: no selection, state=STATE_SCALE_SELECT\n");
    }
}

void CLargeHWView::OnModifyMirror()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"MIRROR");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    m_nLastCommandID = ID_MODIFY_MIRROR;
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyMirror called, selected=%d, entCount=%d\n", nSel, (int)pDoc->GetEntities().size());
    if (nSel > 0) {
        SetDrawState(STATE_MIRROR_P1);
        TRACE(L"[DEBUG] OnModifyMirror: entities pre-selected, state=STATE_MIRROR_P1\n");
    } else {
        SetDrawState(STATE_MIRROR_SELECT);
        TRACE(L"[DEBUG] OnModifyMirror: no selection, state=STATE_MIRROR_SELECT\n");
    }
}

void CLargeHWView::OnModifyOffset()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"OFFSET");
    m_tempPts.clear();
    m_nLastCommandID = ID_MODIFY_OFFSET;
    TRACE(L"[DEBUG] OnModifyOffset called, state=STATE_OFFSET_SELECT\n");
    SetDrawState(STATE_OFFSET_SELECT);
}

// ============================================================
// Edit commands
// ============================================================
void CLargeHWView::OnEditUndo()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"UNDO");
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->Undo();
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::OnEditRedo()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"REDO");
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->Redo();
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::OnUpdateEditUndo(CCmdUI* pCmdUI)
{
    CLargeHWDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc && pDoc->CanUndo());
}

void CLargeHWView::OnUpdateEditRedo(CCmdUI* pCmdUI)
{
    CLargeHWDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc && pDoc->CanRedo());
}

void CLargeHWView::OnEditSelectAll()
{
    CLargeHWDoc* pDoc = GetDocument();
    for (auto* p : pDoc->GetEntities())
        const_cast<CEntity*>(p)->m_bSelected = true;
    Invalidate(FALSE);
}

// ============================================================
// View commands
// ============================================================
void CLargeHWView::OnViewZoomExtents()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ZOOME");
    CLargeHWDoc* pDoc = GetDocument();
    const auto& ents = pDoc->GetEntities();
    if (ents.empty()) return;

    CRect bounds(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
    for (const auto* p : ents) {
        CRect eb = const_cast<CEntity*>(p)->GetBounds();
        if (eb.left < bounds.left) bounds.left = eb.left;
        if (eb.top < bounds.top) bounds.top = eb.top;
        if (eb.right > bounds.right) bounds.right = eb.right;
        if (eb.bottom > bounds.bottom) bounds.bottom = eb.bottom;
    }
    if (bounds.left == INT_MAX) return;
    bounds.InflateRect(50, 50);

    CRect rcClient;
    GetClientRect(&rcClient);

    double sx = (double)rcClient.Width() / bounds.Width();
    double sy = (double)rcClient.Height() / bounds.Height();
    pDoc->m_dScale = min(sx, sy);
    pDoc->m_ptOffset = CPoint(
        (int)(-bounds.left * pDoc->m_dScale),
        (int)(-bounds.top * pDoc->m_dScale)
    );
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::OnViewZoomWindow()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ZOOM");
    m_tempPts.clear();
    m_nLastCommandID = ID_VIEW_ZOOM_WINDOW;
    SetDrawState(STATE_ZOOM_WINDOW_P1);
}

void CLargeHWView::OnViewPan()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"PAN");
    m_nLastCommandID = ID_VIEW_PAN;
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_strCommandPrompt = L"PAN: Hold and drag middle mouse button to pan. Press Esc to cancel.";
    UpdateStatusBar();
}

void CLargeHWView::OnViewGrid()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"GRID");
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_bShowGrid = !pDoc->m_bShowGrid;
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::OnViewSnap()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"SNAP");
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_bSnapToGrid = !pDoc->m_bSnapToGrid;
    UpdateStatusBar();
}

void CLargeHWView::OnViewOrtho()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ORTHO");
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_bOrthoMode = !pDoc->m_bOrthoMode;
    UpdateStatusBar();
}

// ============================================================
// Property commands
// ============================================================
void CLargeHWView::OnColorRed()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR RED");     CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(255, 50, 50));   m_currentColor = RGB(255, 50, 50);   UpdateStatusBar(); }
void CLargeHWView::OnColorYellow()  { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR YELLOW");  CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(255, 255, 50));  m_currentColor = RGB(255, 255, 50);  UpdateStatusBar(); }
void CLargeHWView::OnColorGreen()   { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR GREEN");   CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(50, 255, 50));   m_currentColor = RGB(50, 255, 50);   UpdateStatusBar(); }
void CLargeHWView::OnColorCyan()    { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR CYAN");    CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(0, 255, 209));   m_currentColor = RGB(0, 255, 209);   UpdateStatusBar(); }
void CLargeHWView::OnColorBlue()    { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR BLUE");    CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(50, 100, 255));  m_currentColor = RGB(50, 100, 255);  UpdateStatusBar(); }
void CLargeHWView::OnColorMagenta() { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR MAGENTA"); CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(255, 0, 127));   m_currentColor = RGB(255, 0, 127);   UpdateStatusBar(); }
void CLargeHWView::OnColorWhite()   { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR WHITE");   CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(240, 240, 240)); m_currentColor = RGB(240, 240, 240); UpdateStatusBar(); }

void CLargeHWView::OnLinetypeSolid()   { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINETYPE SOLID");   CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_SOLID);   m_currentLineStyle = PS_SOLID;   UpdateStatusBar(); }
void CLargeHWView::OnLinetypeDash()    { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINETYPE DASH");    CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_DASH);    m_currentLineStyle = PS_DASH;    UpdateStatusBar(); }
void CLargeHWView::OnLinetypeDot()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINETYPE DOT");     CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_DOT);     m_currentLineStyle = PS_DOT;     UpdateStatusBar(); }
void CLargeHWView::OnLinetypeDashDot() { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINETYPE DASHDOT"); CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_DASHDOT); m_currentLineStyle = PS_DASHDOT; UpdateStatusBar(); }

void CLargeHWView::OnLineweight1()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINEWEIGHT 1"); CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(1); m_currentLineWidth = 1; UpdateStatusBar(); }
void CLargeHWView::OnLineweight2()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINEWEIGHT 2"); CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(2); m_currentLineWidth = 2; UpdateStatusBar(); }
void CLargeHWView::OnLineweight3()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINEWEIGHT 3"); CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(3); m_currentLineWidth = 3; UpdateStatusBar(); }
void CLargeHWView::OnLineweight4()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINEWEIGHT 4"); CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(4); m_currentLineWidth = 4; UpdateStatusBar(); }

void CLargeHWView::OnCancelCommand()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ESC");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->DeselectAll();
    CompleteDrawCommand();
    Invalidate(FALSE);
}

// ============================================================
// Format commands
// ============================================================
void CLargeHWView::OnFormatLayer()
{
    CLargeHWDoc* pDoc = GetDocument();
    const auto& layers = pDoc->GetLayers();

    CMenu menu;
    menu.CreatePopupMenu();
    for (size_t i = 0; i < layers.size(); ++i) {
        UINT flags = MF_STRING;
        if (layers[i] == pDoc->GetCurrentLayer())
            flags |= MF_CHECKED;
        menu.AppendMenu(flags, 10000 + (UINT)i, layers[i]);
    }
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, 10099, L"New Layer...");

    CPoint pt;
    GetCursorPos(&pt);

    int nSel = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, this);
    menu.DestroyMenu();

    if (nSel >= 10000 && nSel < 10099) {
        int idx = nSel - 10000;
        if (idx < (int)layers.size()) {
            pDoc->SetCurrentLayer(layers[idx]);
            pDoc->m_strCommandPrompt.Format(L"Current layer: %s", (LPCTSTR)layers[idx]);
        }
    } else if (nSel == 10099) {
        CString strName;
        strName.Format(L"Layer%d", (int)layers.size() + 1);
        pDoc->AddLayer(strName);
        pDoc->SetCurrentLayer(strName);
        pDoc->m_strCommandPrompt.Format(L"New layer created: %s", (LPCTSTR)strName);
    }
    UpdateStatusBar();
}

// ============================================================
// SCR script commands
// ============================================================
void CLargeHWView::OnScriptRun()
{
    CFileDialog dlg(TRUE, L"scr", nullptr,
                    OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                    L"SCR Script Files (*.scr)|*.scr|Text Files (*.txt)|*.txt|All Files (*.*)|*.*||",
                    this);
    if (dlg.DoModal() != IDOK)
        return;

    if (!ExecuteScriptFile(dlg.GetPathName())) {
        AfxMessageBox(L"Failed to read SCR script file.", MB_ICONERROR);
    }
}

void CLargeHWView::OnScriptRecordStart()
{
    if (m_bScriptRecording)
        return;

    CFileDialog dlg(FALSE, L"scr", L"recording.scr",
                    OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY,
                    L"SCR Script Files (*.scr)|*.scr|Text Files (*.txt)|*.txt|All Files (*.*)|*.*||",
                    this);
    if (dlg.DoModal() != IDOK)
        return;

    CString strPath = dlg.GetPathName();
    if (!m_scriptRecordFile.Open(strPath,
                                 CFile::modeCreate | CFile::modeWrite | CFile::shareDenyWrite)) {
        AfxMessageBox(L"Failed to create SCR script file.", MB_ICONERROR);
        return;
    }

    m_strScriptRecordPath = strPath;
    m_bScriptRecording = true;

    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc)
        pDoc->m_strCommandPrompt = L"SCR recording started. Use Script > Stop Recording to finish.";
    SyncCommandLinePrompt();
    UpdateStatusBar();
}

void CLargeHWView::OnScriptRecordStop()
{
    if (!m_bScriptRecording)
        return;

    m_scriptRecordFile.Close();
    m_bScriptRecording = false;

    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc)
        pDoc->m_strCommandPrompt.Format(L"SCR recording saved: %s", (LPCTSTR)m_strScriptRecordPath);
    SyncCommandLinePrompt();
    UpdateStatusBar();
}

void CLargeHWView::OnUpdateScriptRun(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(!m_bRunningScript);
}

void CLargeHWView::OnUpdateScriptRecordStart(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(!m_bScriptRecording && !m_bRunningScript);
}

void CLargeHWView::OnUpdateScriptRecordStop(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(m_bScriptRecording);
}

void CLargeHWView::OnContextRepeat()
{
    RepeatLastCommand();
}

void CLargeHWView::RepeatLastCommand()
{
    if (m_nLastCommandID == 0) return;
    switch (m_nLastCommandID) {
    case ID_DRAW_LINE:       OnDrawLine(); break;
    case ID_DRAW_POLYLINE:   OnDrawPolyline(); break;
    case ID_DRAW_CIRCLE:     OnDrawCircle(); break;
    case ID_DRAW_ARC:        OnDrawArc(); break;
    case ID_DRAW_RECTANGLE:  OnDrawRectangle(); break;
    case ID_DRAW_POLYGON:    OnDrawPolygon(); break;
    case ID_DRAW_ELLIPSE:    OnDrawEllipse(); break;
    case ID_DRAW_TEXT:       OnDrawText(); break;
    case ID_MODIFY_MOVE:     OnModifyMove(); break;
    case ID_MODIFY_COPY:     OnModifyCopy(); break;
    case ID_MODIFY_ROTATE:   OnModifyRotate(); break;
    case ID_MODIFY_SCALE:    OnModifyScale(); break;
    case ID_MODIFY_MIRROR:   OnModifyMirror(); break;
    case ID_MODIFY_OFFSET:   OnModifyOffset(); break;
    case ID_VIEW_ZOOM_WINDOW: OnViewZoomWindow(); break;
    default: break;
    }
}

// ============================================================
// SCR script helpers
// ============================================================
CString CLargeHWView::FormatScriptPoint(CPoint pt) const
{
    CString str;
    str.Format(L"%d,%d", pt.x, pt.y);
    return str;
}

CString CLargeHWView::EscapeScriptText(const CString& strText) const
{
    CString str = strText;
    str.Replace(L"\"", L"\\\"");
    return str;
}

void CLargeHWView::RecordScriptInput(const CString& strInput)
{
    if (!m_bScriptRecording || m_bRunningScript)
        return;

    CString strLine = strInput;
    strLine.TrimRight(L"\r\n");

    CStringA utf8 = WideToUtf8(strLine);
    if (!utf8.IsEmpty())
        m_scriptRecordFile.Write((LPCSTR)utf8, (UINT)utf8.GetLength());
    m_scriptRecordFile.Write("\r\n", 2);
    m_scriptRecordFile.Flush();
}

bool CLargeHWView::IsCoordinateInput(const CString& strInput) const
{
    CString str = strInput;
    str.Trim();
    if (str.IsEmpty()) return false;

    if (str.Find(L',') >= 0 || str[0] == L'@' || str.Find(L'<') >= 0)
        return true;

    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || pDoc->m_drawState == STATE_IDLE)
        return false;

    wchar_t* pEnd = nullptr;
    wcstod(str, &pEnd);
    while (pEnd && *pEnd != L'\0' && iswspace(*pEnd))
        ++pEnd;
    return pEnd && *pEnd == L'\0';
}

void CLargeHWView::SyncCommandLinePrompt()
{
    CLargeHWDoc* pDoc = GetDocument();
    CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
    if (!pDoc || !pFrame || !pFrame->m_wndCmdLine.GetSafeHwnd())
        return;

    CString strPrompt = pDoc->m_strCommandPrompt;
    if (strPrompt.IsEmpty()) strPrompt = L"Command: ";
    pFrame->m_wndCmdLine.SetWindowText(strPrompt);
    pFrame->m_wndCmdLine.SetSel(strPrompt.GetLength(), strPrompt.GetLength());
}

void CLargeHWView::SubmitCommandLineInput(const CString& strInput, bool bRecord)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    CString strCmd = strInput;
    strCmd.Trim();

    if (strCmd.IsEmpty()) {
        if (bRecord) RecordScriptInput(L"");
        m_bSubmittingCommandLine = true;
        OnKeyDown(VK_RETURN, 1, 0);
        m_bSubmittingCommandLine = false;
    } else {
        CString strUpper = strCmd;
        strUpper.MakeUpper();

        if (pDoc->m_drawState == STATE_DRAW_POLYLINE_POINT &&
            (strUpper == L"C" || strUpper == L"CLOSE")) {
            if (bRecord) RecordScriptInput(strUpper);
            m_bPolylineClose = !m_bPolylineClose;
            pDoc->m_strCommandPrompt.Format(
                L"PLINE Specify next point (ENTER to finish) [CLOSE=%s]: ",
                m_bPolylineClose ? L"ON" : L"OFF");
            UpdateStatusBar();
            Invalidate(FALSE);
        } else if (IsCoordinateInput(strCmd)) {
            if (bRecord && pDoc->m_drawState != STATE_DRAW_TEXT_POS)
                RecordScriptInput(strCmd);
            m_bSubmittingCommandLine = true;
            ProcessCoordinateInput(strCmd);
            m_bSubmittingCommandLine = false;
        } else {
            bool bTextStart =
                (strUpper == L"T" || strUpper == L"TEXT" || strUpper == L"DT" || strUpper == L"DTEXT");
            bool bScriptControl =
                (strUpper == L"SCRIPT" || strUpper == L"SCR" ||
                 strUpper == L"SCRIPTREC" || strUpper == L"RECORDSCRIPT" || strUpper == L"SCRREC" ||
                 strUpper == L"SCRIPTSTOP" || strUpper == L"STOPSCRIPT" || strUpper == L"SCRSTOP");
            if (bRecord && !bTextStart && !bScriptControl)
                RecordScriptInput(strCmd);

            m_bSubmittingCommandLine = true;
            ExecuteCommand(strCmd);
            m_bSubmittingCommandLine = false;
        }
    }

    if (pDoc->m_drawState == STATE_IDLE)
        pDoc->m_strCommandPrompt = L"Command: ";
    SyncCommandLinePrompt();
}

bool CLargeHWView::ShouldRecordPointForState(CadDrawState state) const
{
    switch (state) {
    case STATE_DRAW_LINE_P1:
    case STATE_DRAW_LINE_P2:
    case STATE_DRAW_CIRCLE_CENTER:
    case STATE_DRAW_CIRCLE_RADIUS:
    case STATE_DRAW_ARC_P1:
    case STATE_DRAW_ARC_P2:
    case STATE_DRAW_ARC_P3:
    case STATE_DRAW_RECT_P1:
    case STATE_DRAW_RECT_P2:
    case STATE_DRAW_POLYGON_CENTER:
    case STATE_DRAW_POLYGON_RADIUS:
    case STATE_DRAW_ELLIPSE_CENTER:
    case STATE_DRAW_ELLIPSE_RADIUS:
    case STATE_DRAW_POLYLINE_POINT:
    case STATE_MOVE_SELECT:
    case STATE_MOVE_BASE:
    case STATE_MOVE_DEST:
    case STATE_COPY_SELECT:
    case STATE_COPY_BASE:
    case STATE_COPY_DEST:
    case STATE_ROTATE_SELECT:
    case STATE_ROTATE_CENTER:
    case STATE_ROTATE_ANGLE:
    case STATE_SCALE_SELECT:
    case STATE_SCALE_BASE:
    case STATE_SCALE_FACTOR:
    case STATE_MIRROR_SELECT:
    case STATE_MIRROR_P1:
    case STATE_MIRROR_P2:
    case STATE_OFFSET_SELECT:
    case STATE_OFFSET_DIST:
    case STATE_ZOOM_WINDOW_P1:
    case STATE_ZOOM_WINDOW_P2:
        return true;
    default:
        return false;
    }
}

CString CLargeHWView::StripScriptComment(const CString& strLine) const
{
    bool bInQuote = false;
    for (int i = 0; i < strLine.GetLength(); ++i) {
        wchar_t ch = strLine[i];
        if (ch == L'"') {
            bInQuote = !bInQuote;
        } else if (!bInQuote && (ch == L';' || ch == L'#')) {
            return strLine.Left(i);
        }
    }
    return strLine;
}

void CLargeHWView::TokenizeScriptLine(const CString& strLine, std::vector<CString>& tokens) const
{
    tokens.clear();
    CString token;
    bool bInQuote = false;

    for (int i = 0; i < strLine.GetLength(); ++i) {
        wchar_t ch = strLine[i];
        if (bInQuote && ch == L'\\' && i + 1 < strLine.GetLength() && strLine[i + 1] == L'"') {
            token += L'"';
            ++i;
        } else if (ch == L'"') {
            bInQuote = !bInQuote;
        } else if (!bInQuote && iswspace(ch)) {
            if (!token.IsEmpty()) {
                tokens.push_back(token);
                token.Empty();
            }
        } else {
            token += ch;
        }
    }

    if (!token.IsEmpty())
        tokens.push_back(token);
}

bool CLargeHWView::ExecuteDirectScriptCommand(const CString& strLine)
{
    std::vector<CString> tokens;
    TokenizeScriptLine(strLine, tokens);
    if (tokens.empty())
        return true;

    CString cmd = tokens[0];
    cmd.MakeUpper();

    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return true;

    if (cmd == L"TEXT" || cmd == L"T" || cmd == L"DT" || cmd == L"DTEXT") {
        if (tokens.size() < 4)
            return false;

        if (pDoc->m_drawState != STATE_IDLE)
            OnCancelCommand();

        CPoint pos = ParseCoordinate(tokens[1], CPoint(0, 0));
        int nHeight = _wtoi(tokens[2]);
        if (nHeight < 1) nHeight = 20;

        CString strText = tokens[3];
        for (size_t i = 4; i < tokens.size(); ++i) {
            strText += L" ";
            strText += tokens[i];
        }

        CTextEntity* pText = new CTextEntity(pos, strText, nHeight);
        pDoc->AddEntity(pText);
        pDoc->m_strCommandPrompt.Format(L"Script TEXT added: %s", (LPCTSTR)strText);
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if (cmd == L"COLOR" && tokens.size() >= 2) {
        CString arg = tokens[1];
        arg.MakeUpper();

        if (arg == L"RED") OnColorRed();
        else if (arg == L"YELLOW") OnColorYellow();
        else if (arg == L"GREEN") OnColorGreen();
        else if (arg == L"CYAN") OnColorCyan();
        else if (arg == L"BLUE") OnColorBlue();
        else if (arg == L"MAGENTA") OnColorMagenta();
        else if (arg == L"WHITE") OnColorWhite();
        else if (arg.Find(L',') > 0) {
            int nFirst = arg.Find(L',');
            int nSecond = arg.Find(L",", nFirst + 1);
            if (nSecond > nFirst) {
                int r = max(0, min(255, _wtoi(arg.Left(nFirst))));
                int g = max(0, min(255, _wtoi(arg.Mid(nFirst + 1, nSecond - nFirst - 1))));
                int b = max(0, min(255, _wtoi(arg.Mid(nSecond + 1))));
                pDoc->SetCurrentColor(RGB(r, g, b));
                m_currentColor = RGB(r, g, b);
                UpdateStatusBar();
            }
        }
        return true;
    }

    if ((cmd == L"LINETYPE" || cmd == L"LTYPE") && tokens.size() >= 2) {
        CString arg = tokens[1];
        arg.MakeUpper();
        if (arg == L"SOLID") OnLinetypeSolid();
        else if (arg == L"DASH") OnLinetypeDash();
        else if (arg == L"DOT") OnLinetypeDot();
        else if (arg == L"DASHDOT") OnLinetypeDashDot();
        return true;
    }

    if ((cmd == L"LINEWEIGHT" || cmd == L"LWEIGHT") && tokens.size() >= 2) {
        int nWidth = _wtoi(tokens[1]);
        if (nWidth <= 1) OnLineweight1();
        else if (nWidth == 2) OnLineweight2();
        else if (nWidth == 3) OnLineweight3();
        else OnLineweight4();
        return true;
    }

    return false;
}

void CLargeHWView::ExecuteScriptLine(const CString& strLine)
{
    CString line = strLine;
    line.TrimRight(L"\r\n");

    CString trimmed = line;
    trimmed.Trim();
    if (trimmed.IsEmpty()) {
        SubmitCommandLineInput(L"", false);
        return;
    }

    CString withoutComment = StripScriptComment(line);
    withoutComment.Trim();
    if (withoutComment.IsEmpty())
        return;

    if (ExecuteDirectScriptCommand(withoutComment))
        return;

    std::vector<CString> tokens;
    TokenizeScriptLine(withoutComment, tokens);
    for (const CString& token : tokens)
        SubmitCommandLineInput(token, false);
}

bool CLargeHWView::ExecuteScriptFile(const CString& strPath)
{
    CString strContent;
    if (!LoadTextFile(strPath, strContent))
        return false;

    bool bPrevRunning = m_bRunningScript;
    m_bRunningScript = true;

    int nLineCount = 0;
    int nStart = 0;
    while (nStart <= strContent.GetLength()) {
        int nEnd = nStart;
        while (nEnd < strContent.GetLength() &&
               strContent[nEnd] != L'\r' &&
               strContent[nEnd] != L'\n') {
            ++nEnd;
        }

        CString strLine = strContent.Mid(nStart, nEnd - nStart);
        ExecuteScriptLine(strLine);
        ++nLineCount;

        if (nEnd >= strContent.GetLength())
            break;

        nStart = nEnd + 1;
        if (strContent[nEnd] == L'\r' &&
            nStart < strContent.GetLength() &&
            strContent[nStart] == L'\n') {
            ++nStart;
        }
    }

    m_bRunningScript = bPrevRunning;

    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        if (pDoc->m_drawState == STATE_IDLE)
            pDoc->m_strCommandPrompt.Format(L"Script loaded: %d lines", nLineCount);
        else
            pDoc->m_strCommandPrompt.Format(L"Script paused: command needs more input after %d lines", nLineCount);
    }
    SyncCommandLinePrompt();
    UpdateStatusBar();
    Invalidate(FALSE);
    return true;
}

// ============================================================
// Parse coordinate from command line input
//   "100,200"  -> absolute (100, 200)
//   "@50,100"  -> relative to ref point (ref+50, ref+100)
//   "100<45"   -> polar: distance 100 at 45 degrees from ref
// ============================================================
CPoint CLargeHWView::ParseCoordinate(const CString& str, CPoint ref) const
{
    CString s = str;
    s.Trim();

    if (s.IsEmpty()) return ref;

    bool bRelative = false;
    if (s[0] == L'@') {
        bRelative = true;
        s = s.Mid(1);
        s.Trim();
    }

    // Polar: dist<angle
    int nLt = s.Find(L'<');
    if (nLt > 0) {
        double dist = _wtof(s.Left(nLt));
        double angleDeg = _wtof(s.Mid(nLt + 1));
        double angleRad = angleDeg * M_PI / 180.0;
        return CPoint(ref.x + (int)(dist * cos(angleRad)),
                      ref.y + (int)(dist * sin(angleRad)));
    }

    // Cartesian: x,y
    int nComma = s.Find(L',');
    if (nComma > 0) {
        int x = _wtoi(s.Left(nComma));
        int y = _wtoi(s.Mid(nComma + 1));
        if (bRelative) return CPoint(ref.x + x, ref.y + y);
        return CPoint(x, y);
    }

    // Single number: treat as x-coordinate with y=0, or distance if relative
    int val = _wtoi(s);
    if (bRelative) return CPoint(ref.x + val, ref.y);
    return CPoint(val, ref.y);
}

// ============================================================
// Process coordinate input - convert world coords to screen click
// ============================================================
void CLargeHWView::ProcessCoordinateInput(const CString& strInput)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    CadDrawState state = pDoc->m_drawState;

    // Single-number input for distance/angle states
    if (strInput.Find(L',') < 0 && strInput[0] != L'@' && strInput.Find(L'<') < 0) {
        int val = _wtoi(strInput);

        switch (state) {
        case STATE_DRAW_CIRCLE_RADIUS:
            if (!m_tempPts.empty()) {
                int r = abs(val);
                CCircleEntity* pCir = new CCircleEntity(m_tempPts[0], r);
                pDoc->AddEntity(pCir);
                CompleteDrawCommand();
            }
            return;

        case STATE_DRAW_POLYGON_RADIUS:
            if (!m_tempPts.empty()) {
                int r = abs(val);
                CPolygonEntity* pPoly = new CPolygonEntity(m_tempPts[0], r, m_nPolygonSides);
                pDoc->AddEntity(pPoly);
                CompleteDrawCommand();
            }
            return;

        case STATE_DRAW_ELLIPSE_RADIUS:
            if (!m_tempPts.empty()) {
                int r = abs(val);
                CEllipseEntity* pEll = new CEllipseEntity(m_tempPts[0], r, r);
                pDoc->AddEntity(pEll);
                CompleteDrawCommand();
            }
            return;

        case STATE_ROTATE_ANGLE:
            if (!m_tempPts.empty()) {
                double angle = val * M_PI / 180.0;
                int nCount = pDoc->GetSelectedCount();
                if (nCount > 0) {
                    pDoc->RecordModifyUndo();
                    pDoc->RotateSelected(m_tempPts.back(), angle);
                    pDoc->m_strCommandPrompt.Format(L"Rotated %d entities (angle=%.1f deg)", nCount, (double)val);
                }
                CompleteDrawCommand();
            }
            return;

        case STATE_SCALE_FACTOR:
            if (!m_tempPts.empty()) {
                double factor = (double)val;
                if (factor == 0) factor = 1.0;
                int nCount = pDoc->GetSelectedCount();
                if (nCount > 0) {
                    pDoc->RecordModifyUndo();
                    pDoc->ScaleSelected(m_tempPts.back(), factor);
                    pDoc->m_strCommandPrompt.Format(L"Scaled %d entities (factor=%.2f)", nCount, factor);
                }
                CompleteDrawCommand();
            }
            return;
        }
    }

    // Coordinate input (comma / @ / polar) - convert world -> screen -> click
    CPoint refPt(0, 0);
    if (!m_tempPts.empty()) refPt = m_tempPts.back();

    CPoint world = ParseCoordinate(strInput, refPt);

    // Apply snap
    world = SnapToGrid(world);
    if (pDoc->m_bObjectSnap) world = SnapToObjects(world);

    // Convert to screen and inject as click
    CPoint screen = WorldToScreen(world);
    m_ptCurrent = screen;
    OnLButtonDown(0, screen);
}

// ============================================================
// Execute command from command line (AutoCAD-style aliases)
// ============================================================
void CLargeHWView::ExecuteCommand(const CString& strCmd)
{
    if (strCmd.IsEmpty()) return;

    CString cmd = strCmd;
    cmd.MakeUpper();
    cmd.Trim();

    // Cancel current command first if in progress
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    // Commands that cancel ongoing operations
    if (cmd == L"ESC" || cmd == L"CANCEL" || cmd == L"*CANCEL*") {
        OnCancelCommand();
        return;
    }

    // If currently in a command, cancel first before starting new one
    if (pDoc->m_drawState != STATE_IDLE) {
        OnCancelCommand();
    }

    // --- Draw commands ---
    if (cmd == L"L" || cmd == L"LINE") {
        OnDrawLine();
    } else if (cmd == L"C" || cmd == L"CIRCLE") {
        OnDrawCircle();
    } else if (cmd == L"A" || cmd == L"ARC") {
        OnDrawArc();
    } else if (cmd == L"REC" || cmd == L"RECTANG" || cmd == L"RECTANGLE") {
        OnDrawRectangle();
    } else if (cmd == L"PL" || cmd == L"PLINE" || cmd == L"POLYLINE") {
        OnDrawPolyline();
    } else if (cmd == L"EL" || cmd == L"ELLIPSE") {
        OnDrawEllipse();
    } else if (cmd == L"POL" || cmd == L"POLYGON") {
        OnDrawPolygon();
    } else if (cmd == L"T" || cmd == L"TEXT" || cmd == L"DT" || cmd == L"DTEXT") {
        OnDrawText();

    // --- Modify commands ---
    } else if (cmd == L"M" || cmd == L"MOVE") {
        OnModifyMove();
    } else if (cmd == L"CO" || cmd == L"CP" || cmd == L"COPY") {
        OnModifyCopy();
    } else if (cmd == L"RO" || cmd == L"ROTATE") {
        OnModifyRotate();
    } else if (cmd == L"SC" || cmd == L"SCALE") {
        OnModifyScale();
    } else if (cmd == L"MI" || cmd == L"MIRROR") {
        OnModifyMirror();
    } else if (cmd == L"O" || cmd == L"OFFSET") {
        OnModifyOffset();
    } else if (cmd == L"E" || cmd == L"ERASE" || cmd == L"DEL" || cmd == L"DELETE") {
        OnModifyDelete();

    // --- View commands ---
    } else if (cmd == L"Z" || cmd == L"ZOOM") {
        OnViewZoomWindow();
    } else if (cmd == L"ZE" || cmd == L"ZOOME") {
        OnViewZoomExtents();
    } else if (cmd == L"P" || cmd == L"PAN") {
        OnViewPan();

    // --- Edit commands ---
    } else if (cmd == L"U" || cmd == L"UNDO") {
        OnEditUndo();
    } else if (cmd == L"REDO") {
        OnEditRedo();

    // --- Scripts ---
    } else if (cmd == L"SCRIPT" || cmd == L"SCR") {
        OnScriptRun();
    } else if (cmd == L"SCRIPTREC" || cmd == L"RECORDSCRIPT" || cmd == L"SCRREC") {
        OnScriptRecordStart();
    } else if (cmd == L"SCRIPTSTOP" || cmd == L"STOPSCRIPT" || cmd == L"SCRSTOP") {
        OnScriptRecordStop();

    // --- Toggles ---
    } else if (cmd == L"GRID" || cmd == L"F7") {
        pDoc->m_bShowGrid = !pDoc->m_bShowGrid;
        UpdateStatusBar();
        Invalidate(FALSE);
    } else if (cmd == L"SNAP" || cmd == L"F9") {
        pDoc->m_bSnapToGrid = !pDoc->m_bSnapToGrid;
        UpdateStatusBar();
        Invalidate(FALSE);
    } else if (cmd == L"ORTHO" || cmd == L"F8") {
        pDoc->m_bOrthoMode = !pDoc->m_bOrthoMode;
        UpdateStatusBar();
        Invalidate(FALSE);
    } else if (cmd == L"OSNAP" || cmd == L"F3") {
        pDoc->m_bObjectSnap = !pDoc->m_bObjectSnap;
        UpdateStatusBar();
        Invalidate(FALSE);

    } else if (ExecuteDirectScriptCommand(strCmd)) {
        return;

    // --- Unknown command ---
    } else {
        pDoc->m_strCommandPrompt.Format(L"Unknown command: \"%s\". Press F1 for help.", (LPCTSTR)strCmd);
        CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
        if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
            pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
            int nLen = pDoc->m_strCommandPrompt.GetLength();
            pFrame->m_wndCmdLine.SetSel(nLen, nLen);
        }
    }
}

// ============================================================
// Print support
// ============================================================
BOOL CLargeHWView::OnPreparePrinting(CPrintInfo* pInfo)
{
    return DoPreparePrinting(pInfo);
}

void CLargeHWView::OnBeginPrinting(CDC* pDC, CPrintInfo* pInfo)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    CRect rcClient;
    GetClientRect(&rcClient);

    const auto& ents = pDoc->GetEntities();
    if (ents.empty()) return;

    CRect bounds(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
    for (const auto* p : ents) {
        CRect eb = const_cast<CEntity*>(p)->GetBounds();
        if (eb.left < bounds.left) bounds.left = eb.left;
        if (eb.top < bounds.top) bounds.top = eb.top;
        if (eb.right > bounds.right) bounds.right = eb.right;
        if (eb.bottom > bounds.bottom) bounds.bottom = eb.bottom;
    }
    if (bounds.left == INT_MAX) return;
    bounds.InflateRect(50, 50);

    int pageW = pDC->GetDeviceCaps(HORZRES);
    int pageH = pDC->GetDeviceCaps(VERTRES);

    double sx = (double)pageW / bounds.Width();
    double sy = (double)pageH / bounds.Height();
    double printScale = min(sx, sy);

    CPoint printOffset(
        (int)(-bounds.left * printScale),
        (int)(-bounds.top * printScale)
    );

    double saveScale = pDoc->m_dScale;
    CPoint saveOffset = pDoc->m_ptOffset;

    pDoc->m_dScale = printScale;
    pDoc->m_ptOffset = printOffset;

    OnDraw(pDC);

    pDoc->m_dScale = saveScale;
    pDoc->m_ptOffset = saveOffset;
}

void CLargeHWView::OnEndPrinting(CDC* /*pDC*/, CPrintInfo* /*pInfo*/) {}

// ============================================================
// Diagnostics
// ============================================================
#ifdef _DEBUG
void CLargeHWView::AssertValid() const { CView::AssertValid(); }
void CLargeHWView::Dump(CDumpContext& dc) const { CView::Dump(dc); }

CLargeHWDoc* CLargeHWView::GetDocument() const
{
    ASSERT(m_pDocument->IsKindOf(RUNTIME_CLASS(CLargeHWDoc)));
    return (CLargeHWDoc*)m_pDocument;
}
#endif
