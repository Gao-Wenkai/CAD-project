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

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

IMPLEMENT_DYNCREATE(CLargeHWView, CView)

BEGIN_MESSAGE_MAP(CLargeHWView, CView)
    ON_COMMAND(ID_FILE_PRINT, &CView::OnFilePrint)
    ON_COMMAND(ID_FILE_PRINT_DIRECT, &CView::OnFilePrint)
    ON_COMMAND(ID_FILE_PRINT_PREVIEW, &CView::OnFilePrintPreview)

    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSEMOVE()
    ON_WM_RBUTTONDOWN()
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

    ON_COMMAND(ID_CAD_UNDO,       &CLargeHWView::OnEditUndo)
    ON_COMMAND(ID_CAD_REDO,       &CLargeHWView::OnEditRedo)
    ON_COMMAND(ID_EDIT_SELECTALL,  &CLargeHWView::OnEditSelectAll)

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
    , m_currentColor(RGB(0, 255, 209))
    , m_currentLineStyle(PS_SOLID)
    , m_currentLineWidth(1)
{
}

CLargeHWView::~CLargeHWView() {}

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

    // Preview (rubber band)
    if (m_bDrawing) DrawPreview(pDC);

    // UCS icon
    DrawUCSIcon(pDC);

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
    for (auto* p : pDoc->GetEntities())
        const_cast<CEntity*>(p)->Draw(pDC, pDoc->m_dScale, pDoc->m_ptOffset);
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

    CPoint curWorld = ScreenToWorld(m_ptCurrent);

    switch (pDoc->m_drawState) {
    case STATE_DRAW_LINE_P2:
    case STATE_DRAW_RECT_P2:
        if (m_tempPts.size() >= 1) {
            CPoint p1 = WorldToScreen(m_tempPts[0]);
            pDC->MoveTo(p1);
            pDC->LineTo(m_ptCurrent);
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
            pDC->LineTo(m_ptCurrent);
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
            CPoint base = WorldToScreen(m_tempPts[0]);
            pDC->MoveTo(base);
            pDC->LineTo(m_ptCurrent);
        }
        break;

    case STATE_WINDOW_SELECT: {
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
    strCoord.Format(L"X: %d  Y: %d  | Zoom: %.2f  |  %s  SNAP=%s GRID=%s ORTHO=%s",
                    world.x, world.y, pDoc->m_dScale,
                    (LPCTSTR)pDoc->m_strCommandPrompt,
                    pDoc->m_bSnapToGrid ? L"ON" : L"OFF",
                    pDoc->m_bShowGrid ? L"ON" : L"OFF",
                    pDoc->m_bOrthoMode ? L"ON" : L"OFF");

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
    pDoc->DeselectAll();

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
    case STATE_IDLE:
    default:                         pDoc->m_strCommandPrompt = L"Command: "; m_bDrawing = false; break;
    }
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::CompleteDrawCommand()
{
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
    CadDrawState state = pDoc->m_drawState;

    // Check grip hit first (only in IDLE state)
    if (state == STATE_IDLE) {
        CEntity* hitEntity = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hitEntity && hitEntity->m_bSelected) {
            int gripIdx = hitEntity->HitTestGrip(point, pDoc->m_dScale, pDoc->m_ptOffset);
            if (gripIdx >= 0) {
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
            pDoc->DeselectAll();
            pDoc->m_drawState = STATE_WINDOW_SELECT;
            m_ptDragStart = point;
            m_bDragging = true;
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
        CTextEntity* pText = new CTextEntity(world, L"CAD Text", 20);
        pDoc->AddEntity(pText);
        CompleteDrawCommand();
        break;
    }

    case STATE_MOVE_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            m_tempPts.push_back(world);
            SetDrawState(STATE_MOVE_BASE);
        }
        break;
    }

    case STATE_MOVE_BASE:
        m_tempPts.push_back(world);
        SetDrawState(STATE_MOVE_DEST);
        break;

    case STATE_MOVE_DEST: {
        double dx = world.x - m_tempPts[1].x;
        double dy = world.y - m_tempPts[1].y;
        pDoc->MoveSelected(dx, dy);
        pDoc->DeselectAll();
        CompleteDrawCommand();
        break;
    }

    case STATE_COPY_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            m_tempPts.push_back(world);
            SetDrawState(STATE_COPY_BASE);
        }
        break;
    }

    case STATE_COPY_BASE:
        m_tempPts.push_back(world);
        SetDrawState(STATE_COPY_DEST);
        break;

    case STATE_COPY_DEST: {
        double dx = world.x - m_tempPts[1].x;
        double dy = world.y - m_tempPts[1].y;
        auto selectedEnts = pDoc->GetSelectedEntities();
        for (auto* pEnt : selectedEnts) {
            CEntity* pCopy = pEnt->Clone();
            pCopy->Move(dx, dy);
            pDoc->AddEntity(pCopy);
        }
        pDoc->DeselectAll();
        CompleteDrawCommand();
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

    if (m_bDragging && m_nGripIndex >= 0 && m_pGripEntity) {
        CPoint world = ScreenToWorld(point);
        m_pGripEntity->SetGrip(m_nGripIndex, world);
        m_ptCurrent = point;
        pDoc->SetModified(true);
        UpdateStatusBar();
        Invalidate(FALSE);
        return;
    }

    m_ptCurrent = point;
    UpdateStatusBar();

    if (pDoc->m_drawState == STATE_WINDOW_SELECT && m_bDragging) {
        Invalidate(FALSE);
        return;
    }

    if (m_bDrawing) {
        Invalidate(FALSE);
    }

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
            CPolylineEntity* pPline = new CPolylineEntity(m_tempPts);
            pPline->m_bClosed = m_bPolylineClose;
            pDoc->AddEntity(pPline);
            m_bPolylineClose = false;
            CompleteDrawCommand();
            Invalidate(FALSE);
        }
        if (pDoc->m_drawState == STATE_DRAW_ARC_PREVIEW && m_tempPts.size() >= 3) {
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
        pDoc->DeleteSelected();
        Invalidate(FALSE);
        break;

    case 'Z':
        if (GetKeyState(VK_CONTROL) & 0x8000)
            pDoc->Undo();
        break;

    case 'Y':
        if (GetKeyState(VK_CONTROL) & 0x8000)
            pDoc->Redo();
        break;

    case 'A':
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            for (auto* p : pDoc->GetEntities())
                const_cast<CEntity*>(p)->m_bSelected = true;
            Invalidate(FALSE);
        }
        break;

    case VK_F7:
        pDoc->m_bShowGrid = !pDoc->m_bShowGrid;
        UpdateStatusBar();
        Invalidate(FALSE);
        break;

    case VK_F9:
        pDoc->m_bSnapToGrid = !pDoc->m_bSnapToGrid;
        UpdateStatusBar();
        Invalidate(FALSE);
        break;

    case VK_F8:
        pDoc->m_bOrthoMode = !pDoc->m_bOrthoMode;
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
void CLargeHWView::OnDrawLine()       { m_tempPts.clear(); SetDrawState(STATE_DRAW_LINE_P1); }
void CLargeHWView::OnDrawCircle()     { m_tempPts.clear(); SetDrawState(STATE_DRAW_CIRCLE_CENTER); }
void CLargeHWView::OnDrawArc()        { m_tempPts.clear(); SetDrawState(STATE_DRAW_ARC_P1); }
void CLargeHWView::OnDrawRectangle()  { m_tempPts.clear(); SetDrawState(STATE_DRAW_RECT_P1); }
void CLargeHWView::OnDrawEllipse()    { m_tempPts.clear(); SetDrawState(STATE_DRAW_ELLIPSE_CENTER); }
void CLargeHWView::OnDrawPolyline()   { m_tempPts.clear(); m_bPolylineClose = false; SetDrawState(STATE_DRAW_POLYLINE_POINT); }
void CLargeHWView::OnDrawText()       { m_tempPts.clear(); SetDrawState(STATE_DRAW_TEXT_POS); }
void CLargeHWView::OnDrawPolygon()    { m_tempPts.clear(); m_nPolygonSides = 6; SetDrawState(STATE_DRAW_POLYGON_CENTER); }

// ============================================================
// Modify command handlers
// ============================================================
void CLargeHWView::OnModifyMove()
{
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc->GetSelectedCount() > 0) {
        m_tempPts.push_back(CPoint(0, 0));
        SetDrawState(STATE_MOVE_BASE);
    } else {
        SetDrawState(STATE_MOVE_SELECT);
    }
}

void CLargeHWView::OnModifyCopy()
{
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc->GetSelectedCount() > 0) {
        m_tempPts.push_back(CPoint(0, 0));
        SetDrawState(STATE_COPY_BASE);
    } else {
        SetDrawState(STATE_COPY_SELECT);
    }
}

void CLargeHWView::OnModifyDelete()
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->DeleteSelected();
    Invalidate(FALSE);
}

void CLargeHWView::OnModifyRotate()  { AfxMessageBox(L"ROTATE: TODO"); }
void CLargeHWView::OnModifyScale()   { AfxMessageBox(L"SCALE: TODO"); }
void CLargeHWView::OnModifyMirror()  { AfxMessageBox(L"MIRROR: TODO"); }
void CLargeHWView::OnModifyOffset()  { AfxMessageBox(L"OFFSET: TODO"); }

// ============================================================
// Edit commands
// ============================================================
void CLargeHWView::OnEditUndo()
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->Undo();
    Invalidate(FALSE);
}

void CLargeHWView::OnEditRedo()
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->Redo();
    Invalidate(FALSE);
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

void CLargeHWView::OnViewZoomWindow()   { AfxMessageBox(L"ZOOM Window: TODO"); }
void CLargeHWView::OnViewPan()          { AfxMessageBox(L"PAN: Hold middle mouse to pan"); }

void CLargeHWView::OnViewGrid()
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_bShowGrid = !pDoc->m_bShowGrid;
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::OnViewSnap()
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_bSnapToGrid = !pDoc->m_bSnapToGrid;
    UpdateStatusBar();
}

void CLargeHWView::OnViewOrtho()
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_bOrthoMode = !pDoc->m_bOrthoMode;
    UpdateStatusBar();
}

// ============================================================
// Property commands
// ============================================================
void CLargeHWView::OnColorRed()     { CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(255, 50, 50));   m_currentColor = RGB(255, 50, 50);   UpdateStatusBar(); }
void CLargeHWView::OnColorYellow()  { CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(255, 255, 50));  m_currentColor = RGB(255, 255, 50);  UpdateStatusBar(); }
void CLargeHWView::OnColorGreen()   { CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(50, 255, 50));   m_currentColor = RGB(50, 255, 50);   UpdateStatusBar(); }
void CLargeHWView::OnColorCyan()    { CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(0, 255, 209));   m_currentColor = RGB(0, 255, 209);   UpdateStatusBar(); }
void CLargeHWView::OnColorBlue()    { CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(50, 100, 255));  m_currentColor = RGB(50, 100, 255);  UpdateStatusBar(); }
void CLargeHWView::OnColorMagenta() { CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(255, 0, 127));   m_currentColor = RGB(255, 0, 127);   UpdateStatusBar(); }
void CLargeHWView::OnColorWhite()   { CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(240, 240, 240)); m_currentColor = RGB(240, 240, 240); UpdateStatusBar(); }

void CLargeHWView::OnLinetypeSolid()   { CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_SOLID);   m_currentLineStyle = PS_SOLID;   UpdateStatusBar(); }
void CLargeHWView::OnLinetypeDash()    { CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_DASH);    m_currentLineStyle = PS_DASH;    UpdateStatusBar(); }
void CLargeHWView::OnLinetypeDot()     { CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_DOT);     m_currentLineStyle = PS_DOT;     UpdateStatusBar(); }
void CLargeHWView::OnLinetypeDashDot() { CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_DASHDOT); m_currentLineStyle = PS_DASHDOT; UpdateStatusBar(); }

void CLargeHWView::OnLineweight1()     { CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(1); m_currentLineWidth = 1; UpdateStatusBar(); }
void CLargeHWView::OnLineweight2()     { CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(2); m_currentLineWidth = 2; UpdateStatusBar(); }
void CLargeHWView::OnLineweight3()     { CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(3); m_currentLineWidth = 3; UpdateStatusBar(); }
void CLargeHWView::OnLineweight4()     { CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(4); m_currentLineWidth = 4; UpdateStatusBar(); }

void CLargeHWView::OnCancelCommand()
{
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->DeselectAll();
    CompleteDrawCommand();
    Invalidate(FALSE);
}

// ============================================================
// Print support
// ============================================================
BOOL CLargeHWView::OnPreparePrinting(CPrintInfo* pInfo)
{
    return DoPreparePrinting(pInfo);
}

void CLargeHWView::OnBeginPrinting(CDC* /*pDC*/, CPrintInfo* /*pInfo*/) {}
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
