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
#include "Dimension.h"
#include "MainFrm.h"
#include <map>
#include <algorithm>
#include <vector>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// Helper: closest point on segment to a given point (world coords)
static CPoint ClosestPointOnSegment(const CPoint& s, const CPoint& e, const CPoint& p)
{
    double vx = (double)(e.x - s.x), vy = (double)(e.y - s.y);
    double wx = (double)(p.x - s.x), wy = (double)(p.y - s.y);
    double denom = vx*vx + vy*vy;
    if (denom <= 1e-9) return s;
    double t = (vx*wx + vy*wy) / denom;
    if (t < 0) t = 0; if (t > 1) t = 1;
    return CPoint((int)round(s.x + t*vx), (int)round(s.y + t*vy));
}

// Helper: split line/polyline entities at pairwise intersections and replace originals with pieces
// Internal helper: create temporary splits for angle-dim; returns created IDs and stores originals as clones
static void CreateTemporarySplits(CLargeHWDoc* pDoc, std::vector<int>& outNewIDs)
{
    if (!pDoc) return;
    // collect segments: each entry holds endpoints and owner info
    struct Seg { CPoint a, b; CEntity* owner; int segIndex; bool ownerIsPolyline; };
    std::vector<Seg> segs;
    // snapshot entities list to avoid mutation during iteration
    auto ents = pDoc->GetEntities();
    for (auto* e : ents) {
        if (auto ln = dynamic_cast<CLineEntity*>(e)) {
            segs.push_back({ln->m_ptStart, ln->m_ptEnd, e, -1, false});
        } else if (auto pl = dynamic_cast<CPolylineEntity*>(e)) {
            int n = (int)pl->m_vertices.size();
            for (int i = 0; i < n - 1; ++i) {
                segs.push_back({pl->m_vertices[i], pl->m_vertices[i+1], e, i, true});
            }
        }
    }

    if (segs.size() < 2) return;

    // for each segment, collect intersection parameters t along 0..1
    std::vector<std::vector<double>> interT(segs.size());
    auto add_inter = [&](int i, double t){ if (t >= 0.0 && t <= 1.0) interT[i].push_back(t); };

    for (size_t i = 0; i < segs.size(); ++i) {
        for (size_t j = i+1; j < segs.size(); ++j) {
            // skip if same owner (we don't split within same polyline by self-intersection here)
            if (segs[i].owner == segs[j].owner) continue;
            // compute intersection of segments i and j
            double x1 = segs[i].a.x, y1 = segs[i].a.y, x2 = segs[i].b.x, y2 = segs[i].b.y;
            double x3 = segs[j].a.x, y3 = segs[j].a.y, x4 = segs[j].b.x, y4 = segs[j].b.y;
            double den = (x1 - x2)*(y3 - y4) - (y1 - y2)*(x3 - x4);
            if (fabs(den) < 1e-6) continue;
            double px = ((x1*y2 - y1*x2)*(x3 - x4) - (x1 - x2)*(x3*y4 - y3*x4)) / den;
            double py = ((x1*y2 - y1*x2)*(y3 - y4) - (y1 - y2)*(x3*y4 - y3*x4)) / den;
            // compute parameter t on each segment
            double ti = (fabs(x2 - x1) > fabs(y2 - y1)) ? ((px - x1) / (x2 - x1)) : ((py - y1) / (y2 - y1));
            double tj = (fabs(x4 - x3) > fabs(y4 - y3)) ? ((px - x3) / (x4 - x3)) : ((py - y3) / (y4 - y3));
            if (ti >= -1e-6 && ti <= 1.0+1e-6 && tj >= -1e-6 && tj <= 1.0+1e-6) {
                add_inter((int)i, max(0.0, min(1.0, ti)));
                add_inter((int)j, max(0.0, min(1.0, tj)));
            }
        }
    }

    // prepare map owner -> created segments
    std::map<CEntity*, std::vector<std::pair<CPoint,CPoint>>> createdMap;

    for (size_t i = 0; i < segs.size(); ++i) {
        auto& seg = segs[i];
        auto& ts = interT[i];
        // if no intersections, skip
        if (ts.empty()) continue;
        // include endpoints 0 and 1
        ts.push_back(0.0); ts.push_back(1.0);
        sort(ts.begin(), ts.end());
        // unique with tolerance
        std::vector<double> uniq;
        for (double t : ts) {
            if (uniq.empty() || fabs(t - uniq.back()) > 1e-6) uniq.push_back(t);
        }
        for (size_t k = 0; k + 1 < uniq.size(); ++k) {
            double t0 = uniq[k], t1 = uniq[k+1];
            if (t1 - t0 < 1e-6) continue;
            // compute piece endpoints
            CPoint p0((int)floor(seg.a.x + (seg.b.x - seg.a.x) * t0 + 0.5), (int)floor(seg.a.y + (seg.b.y - seg.a.y) * t0 + 0.5));
            CPoint p1((int)floor(seg.a.x + (seg.b.x - seg.a.x) * t1 + 0.5), (int)floor(seg.a.y + (seg.b.y - seg.a.y) * t1 + 0.5));
            if (Distance(p0, p1) <= 1e-6) continue;
            createdMap[seg.owner].push_back({p0, p1});
        }
    }

    // Add created pieces as temporary segments (do not remove originals); record new IDs
    for (auto& kv : createdMap) {
        for (auto& pr : kv.second) {
            CLineEntity* ln = new CLineEntity(pr.first, pr.second);
            pDoc->AddEntity(ln);
            outNewIDs.push_back(ln->m_nID);
        }
    }
}

// Restore temporary splits: remove created segments
static void RestoreTemporarySplits(CLargeHWDoc* pDoc, const std::vector<int>& newIDs)
{
    if (!pDoc) return;
    for (int id : newIDs) pDoc->RemoveEntity(id, false);
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
    ON_COMMAND(ID_DRAW_DIMENSION_LENGTH, &CLargeHWView::OnDrawDimLength)
    ON_COMMAND(ID_DRAW_DIMENSION_ANGLE,  &CLargeHWView::OnDrawDimAngle)
    ON_COMMAND(ID_DRAW_DIM_LENGTH_ALIGNED, &CLargeHWView::OnDrawDimLengthAligned)
    ON_COMMAND(ID_DRAW_DIM_LENGTH_HORIZ, &CLargeHWView::OnDrawDimLengthHoriz)
    ON_COMMAND(ID_DRAW_DIM_LENGTH_VERT, &CLargeHWView::OnDrawDimLengthVert)
    ON_COMMAND(ID_DRAW_DIMENSION_RADIUS, &CLargeHWView::OnDrawDimRadius)
    ON_COMMAND(ID_DRAW_DIMENSION_DIAMETER, &CLargeHWView::OnDrawDimDiameter)
    ON_COMMAND(ID_DRAW_DIMENSION_ARCLENGTH, &CLargeHWView::OnDrawDimArcLength)
    ON_COMMAND(ID_DRAW_DIMENSION_COORDINATE, &CLargeHWView::OnDrawDimCoordinate)

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
    , m_pDimEnt1(nullptr)
    , m_pDimEnt2(nullptr)
    , m_pDimRadiusSrcEnt(nullptr)
    , m_pDimDiamSrcEnt(nullptr)
    , m_pDimArcLenSrcEnt(nullptr)
    , m_bCoordDimMode(false)
    , m_ptCoordPoint(0,0)
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
        // draw selection overlay for different entity types (thick cyan stroke)
        for (size_t j = 0; j < ents.size(); ++j) {
            CEntity* e2 = const_cast<CEntity*>(ents[j]);
            if (!e2->m_bSelected) continue;
            // line
            if (auto ln = dynamic_cast<CLineEntity*>(e2)) {
                CPoint p1 = WorldToScreen(ln->m_ptStart);
                CPoint p2 = WorldToScreen(ln->m_ptEnd);
                CPen selPen(PS_SOLID, max(2, ln->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                pDC->MoveTo(p1); pDC->LineTo(p2);
                pDC->SelectObject(pOldPen2);
                continue;
            }
            // polyline
            if (auto pl = dynamic_cast<CPolylineEntity*>(e2)) {
                CPen selPen(PS_SOLID, max(2, pl->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                if (!pl->m_vertices.empty()) {
                    pDC->MoveTo(WorldToScreen(pl->m_vertices[0]));
                    for (size_t k = 1; k < pl->m_vertices.size(); ++k)
                        pDC->LineTo(WorldToScreen(pl->m_vertices[k]));
                }
                pDC->SelectObject(pOldPen2);
                continue;
            }
            // circle
            if (auto cir = dynamic_cast<CCircleEntity*>(e2)) {
                CPoint c = WorldToScreen(cir->m_ptCenter);
                int r2 = (int)(cir->m_nRadius * pDoc->m_dScale);
                if (r2 < 1) r2 = 1;
                CPen selPen(PS_SOLID, max(2, cir->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                CBrush* pOldBr2 = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                pDC->Ellipse(c.x - r2, c.y - r2, c.x + r2, c.y + r2);
                pDC->SelectObject(pOldPen2);
                pDC->SelectObject(pOldBr2);
                continue;
            }
            // polygon
            if (auto poly = dynamic_cast<CPolygonEntity*>(e2)) {
                std::vector<CPoint> verts; poly->GetVertices(verts);
                if (!verts.empty()) {
                    CPen selPen(PS_SOLID, max(2, poly->m_nLineWidth + 2), RGB(0,255,255));
                    CPen* pOldPen2 = pDC->SelectObject(&selPen);
                    pDC->MoveTo(WorldToScreen(verts[0]));
                    for (size_t k = 1; k < verts.size(); ++k) pDC->LineTo(WorldToScreen(verts[k]));
                    pDC->LineTo(WorldToScreen(verts[0]));
                    pDC->SelectObject(pOldPen2);
                }
                continue;
            }
            // rectangle
            if (auto rect = dynamic_cast<CRectangleEntity*>(e2)) {
                CPoint p1 = WorldToScreen(rect->m_ptCorner1);
                CPoint p2 = WorldToScreen(rect->m_ptCorner2);
                int l = min(p1.x, p2.x), r3 = max(p1.x, p2.x);
                int t = min(p1.y, p2.y), b = max(p1.y, p2.y);
                CPen selPen(PS_SOLID, max(2, rect->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                CBrush* pOldBr2 = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                pDC->Rectangle(l, t, r3, b);
                pDC->SelectObject(pOldPen2);
                pDC->SelectObject(pOldBr2);
                continue;
            }
            // ellipse
            if (auto el = dynamic_cast<CEllipseEntity*>(e2)) {
                CPoint c = WorldToScreen(el->m_ptCenter);
                int rx = (int)(el->m_nRadiusX * pDoc->m_dScale);
                int ry = (int)(el->m_nRadiusY * pDoc->m_dScale);
                CPen selPen(PS_SOLID, max(2, el->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                CBrush* pOldBr2 = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                pDC->Ellipse(c.x - rx, c.y - ry, c.x + rx, c.y + ry);
                pDC->SelectObject(pOldPen2);
                pDC->SelectObject(pOldBr2);
                continue;
            }
            // arc (approximate by sampling)
            if (auto ar = dynamic_cast<CArcEntity*>(e2)) {
                CPoint c = WorldToScreen(ar->m_ptCenter);
                int r2 = (int)(ar->m_nRadius * pDoc->m_dScale);
                if (r2 < 1) r2 = 1;
                double angStart = atan2((double)(ar->m_ptStart.y - ar->m_ptCenter.y), (double)(ar->m_ptStart.x - ar->m_ptCenter.x));
                double angEnd   = atan2((double)(ar->m_ptEnd.y - ar->m_ptCenter.y),   (double)(ar->m_ptEnd.x - ar->m_ptCenter.x));
                double sweep = angEnd - angStart; if (sweep < 0) sweep += 2*M_PI;
                int steps = max(8, (int)ceil(sweep / (M_PI/36.0)));
                CPen selPen(PS_SOLID, max(2, ar->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                for (int k = 0; k <= steps; ++k) {
                    double ang = angStart + sweep * (double)k / (double)steps;
                    CPoint p((int)floor(c.x + r2 * cos(ang) + 0.5), (int)floor(c.y + r2 * sin(ang) + 0.5));
                    if (k == 0) pDC->MoveTo(p); else pDC->LineTo(p);
                }
                pDC->SelectObject(pOldPen2);
                continue;
            }
        }
        }
    }
}

// ============================================================
// Draw preview (rubber band)
// ============================================================
void CLargeHWView::DrawPreview(CDC* pDC)
{
    CLargeHWDoc* pDoc = GetDocument();
    TRACE(L"[TRACE] Enter DrawPreview: state=%d, m_bDrawing=%d, tempPts=%d, pDim1=%p pDim2=%p\n",
          (int)pDoc->m_drawState, (int)m_bDrawing, (int)m_tempPts.size(), m_pDimEnt1, m_pDimEnt2);
    if (m_tempPts.empty()) {
        // still allow preview/highlight when in angular-dimension selection states
        if (!(pDoc->m_drawState == STATE_DRAW_DIM_ANGLE_SELECT_E1 ||
              pDoc->m_drawState == STATE_DRAW_DIM_ANGLE_SELECT_E2 ||
              pDoc->m_drawState == STATE_DRAW_DIM_ANGLE_POS ||
              pDoc->m_drawState == STATE_DRAW_DIM_RADIUS_SELECT ||
              pDoc->m_drawState == STATE_DRAW_DIM_RADIUS_POS ||
              pDoc->m_drawState == STATE_DRAW_DIM_DIAMETER_SELECT ||
              pDoc->m_drawState == STATE_DRAW_DIM_DIAMETER_POS ||
              pDoc->m_drawState == STATE_DRAW_DIM_ARCLEN_SELECT ||
              pDoc->m_drawState == STATE_DRAW_DIM_ARCLEN_POS ||
              (pDoc->m_drawState == STATE_DRAW_TEXT_POS && m_pPendingDim)))
            return;
    }

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

    case STATE_DRAW_DIM_LENGTH_P1:
        // waiting for first point; nothing to preview
        break;
    case STATE_DRAW_DIM_LENGTH_P2:
        if (m_tempPts.size() >= 1) {
            CPoint p1 = WorldToScreen(m_tempPts[0]);
            pDC->MoveTo(p1);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_DRAW_DIM_LENGTH_POS:
        // Rubber-band preview for linear dimension placement: show extension lines,
        // main dim line and arrowheads based on current mouse position
        if (m_tempPts.size() >= 2) {
            CPoint a = m_tempPts[0];
            CPoint b = m_tempPts[1];
            // determine mode
            int mode = m_nLastDimMode; // 0=aligned,1=horizontal,2=vertical
            CPoint aDimWorld = a, bDimWorld = b;
            double wx = (double)(b.x - a.x), wy = (double)(b.y - a.y);
            double wlen = sqrt(wx*wx + wy*wy);
            // determine placement (current mouse world)
            CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
            // compute offset in world units
            double screenOffset = 15.0; double offWorld = 1.0;
            CLargeHWDoc* pDocLocal = GetDocument();
            if (pDocLocal && pDocLocal->m_dScale > 0.0) offWorld = screenOffset / pDocLocal->m_dScale;

            if (wlen > 1e-6) {
                if (mode == 1) {
                    // horizontal: y = place.y
                    int left = min(a.x, b.x), right = max(a.x, b.x);
                    aDimWorld = CPoint(left, place.y);
                    bDimWorld = CPoint(right, place.y);
                } else if (mode == 2) {
                    int top = min(a.y, b.y), bottom = max(a.y, b.y);
                    aDimWorld = CPoint(place.x, top);
                    bDimWorld = CPoint(place.x, bottom);
                } else {
                    // aligned mode: compute signed offset from original line to current mouse position
                    double ux = wx, uy = wy;
                    double wlen_d = sqrt(ux*ux + uy*uy);
                    double nx = -uy / wlen_d; double ny = ux / wlen_d; // unit normal
                    CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                    double pax = (double)(place.x - a.x), pay = (double)(place.y - a.y);
                    double signedOff = pax * nx + pay * ny;
                    aDimWorld = CPoint((int)floor((double)a.x + nx * signedOff + 0.5), (int)floor((double)a.y + ny * signedOff + 0.5));
                    bDimWorld = CPoint((int)floor((double)b.x + nx * signedOff + 0.5), (int)floor((double)b.y + ny * signedOff + 0.5));
                }
            } else {
                // degenerate: small offset from a
                aDimWorld = a; bDimWorld = b;
                aDimWorld.y += (int)offWorld; bDimWorld.y += (int)offWorld;
            }

            // screen coords
            CPoint sA = WorldToScreen(a);
            CPoint sB = WorldToScreen(b);
            CPoint sAD = WorldToScreen(aDimWorld);
            CPoint sBD = WorldToScreen(bDimWorld);

            // draw extension/connection lines
            if (mode == 1) {
                CPoint conn1(a.x, aDimWorld.y); CPoint conn2(b.x, bDimWorld.y);
                pDC->MoveTo(sA); pDC->LineTo(WorldToScreen(conn1));
                pDC->MoveTo(sB); pDC->LineTo(WorldToScreen(conn2));
            } else if (mode == 2) {
                CPoint conn1(aDimWorld.x, a.y); CPoint conn2(bDimWorld.x, b.y);
                pDC->MoveTo(sA); pDC->LineTo(WorldToScreen(conn1));
                pDC->MoveTo(sB); pDC->LineTo(WorldToScreen(conn2));
            } else {
                pDC->MoveTo(sA); pDC->LineTo(sAD);
                pDC->MoveTo(sB); pDC->LineTo(sBD);
            }

            // draw main dimension line
            pDC->MoveTo(sAD); pDC->LineTo(sBD);

            // arrowheads
            auto drawArrow = [&](CPoint pt, CPoint toward) {
                double dx = (double)(toward.x - pt.x);
                double dy = (double)(toward.y - pt.y);
                double d = sqrt(dx*dx + dy*dy);
                if (d < 1e-6) return;
                double ux = dx / d, uy = dy / d;
                double ang = atan2(uy, ux);
                double ang1 = ang + M_PI * 3.0 / 8.0;
                double ang2 = ang - M_PI * 3.0 / 8.0;
                int arrowLen = max(8, (int)(8 * pDocLocal->m_dScale));
                CPoint p1((int)floor(pt.x + cos(ang1) * arrowLen + 0.5), (int)floor(pt.y + sin(ang1) * arrowLen + 0.5));
                CPoint p2((int)floor(pt.x + cos(ang2) * arrowLen + 0.5), (int)floor(pt.y + sin(ang2) * arrowLen + 0.5));
                pDC->MoveTo(pt); pDC->LineTo(p1);
                pDC->MoveTo(pt); pDC->LineTo(p2);
            };

            drawArrow(sAD, sBD);
            drawArrow(sBD, sAD);

            // draw length text near middle
            double lenVal;
            if (mode == 1) lenVal = fabs((double)(b.x - a.x));
            else if (mode == 2) lenVal = fabs((double)(b.y - a.y));
            else lenVal = Distance(a, b);
            CString txt; txt.Format(L"%.2f", lenVal);
            CPoint mid((sAD.x + sBD.x)/2, (sAD.y + sBD.y)/2);
            int textOffset = max(6, (int)(6 * pDocLocal->m_dScale));
            double sx = (double)(sBD.x - sAD.x); double sy = (double)(sBD.y - sAD.y);
            double slen = sqrt(sx*sx + sy*sy);
            if (slen > 1e-6) {
                double nxs = -sy / slen; double nys = sx / slen;
                mid.x = (int)floor(mid.x + nxs * textOffset + 0.5);
                mid.y = (int)floor(mid.y + nys * textOffset + 0.5);
            }
            pDC->TextOutW(mid.x+4, mid.y+4, txt);
        }
        break;

    case STATE_DRAW_DIM_ANGLE_SELECT_E1:
        // waiting for first entity selection - highlight under cursor optionally
        // If a polyline segment was identified, draw that segment highlighted
        if (m_pDimEnt1Orig && m_nDimSegIndex1 >= 0) {
            if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
                int i = m_nDimSegIndex1;
                CPoint s = WorldToScreen(pl->m_vertices[i]);
                CPoint e = WorldToScreen(pl->m_vertices[i+1]);
                CPen hl(PS_SOLID, 3, RGB(0,255,255));
                CPen* pOld = pDC->SelectObject(&hl);
                pDC->MoveTo(s); pDC->LineTo(e);
                pDC->SelectObject(pOld);
            }
        }
        break;
    case STATE_DRAW_DIM_ANGLE_SELECT_E2:
        // waiting for second entity selection
        // always draw highlight for first selected segment (if any)
        if (m_pDimEnt1Orig && m_nDimSegIndex1 >= 0) {
            if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
                int i = m_nDimSegIndex1;
                CPoint s = WorldToScreen(pl->m_vertices[i]);
                CPoint e = WorldToScreen(pl->m_vertices[i+1]);
                CPen hl(PS_SOLID, 3, RGB(0,255,255));
                CPen* pOld = pDC->SelectObject(&hl);
                pDC->MoveTo(s); pDC->LineTo(e);
                pDC->SelectObject(pOld);
            }
        }
        if (m_pDimEnt2Orig && m_nDimSegIndex2 >= 0) {
            if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt2Orig)) {
                int i = m_nDimSegIndex2;
                CPoint s = WorldToScreen(pl->m_vertices[i]);
                CPoint e = WorldToScreen(pl->m_vertices[i+1]);
                CPen hl(PS_SOLID, 3, RGB(0,255,255));
                CPen* pOld = pDC->SelectObject(&hl);
                pDC->MoveTo(s); pDC->LineTo(e);
                pDC->SelectObject(pOld);
            }
        }
        break;
    case STATE_DRAW_DIM_ANGLE_POS:
        // preview arc using direct rubber-band drawing (match length-dimension preview style)
        if (m_pDimEnt1 && m_pDimEnt2) {
            // compute center (intersection or midpoint fallback)
            CPoint center;
            CLineEntity* l1 = dynamic_cast<CLineEntity*>(m_pDimEnt1);
            CLineEntity* l2 = dynamic_cast<CLineEntity*>(m_pDimEnt2);
            if (l1 && l2) {
                CPoint a1 = l1->m_ptStart, a2 = l1->m_ptEnd;
                CPoint b1 = l2->m_ptStart, b2 = l2->m_ptEnd;
                double x1 = a1.x, y1 = a1.y, x2 = a2.x, y2 = a2.y;
                double x3 = b1.x, y3 = b1.y, x4 = b2.x, y4 = b2.y;
                double den = (x1 - x2)*(y3 - y4) - (y1 - y2)*(x3 - x4);
                if (fabs(den) < 1e-6) center = MidPoint(a1, a2);
                else {
                    double px = ((x1*y2 - y1*x2)*(x3 - x4) - (x1 - x2)*(x3*y4 - y3*x4)) / den;
                    double py = ((x1*y2 - y1*x2)*(y3 - y4) - (y1 - y2)*(x3*y4 - y3*x4)) / den;
                    center = CPoint((int)px, (int)py);
                }
            } else {
                if (l1) center = MidPoint(l1->m_ptStart, l1->m_ptEnd);
                else if (l2) center = MidPoint(l2->m_ptStart, l2->m_ptEnd);
                else center = CPoint(0,0);
            }

            // compute base angles from segment midpoints to avoid zero-angle when center lies on the lines
            auto vec = [](CPoint a, CPoint b){ return CPoint(b.x - a.x, b.y - a.y); };
            auto dot = [](CPoint u, CPoint v){ return (double)u.x * v.x + (double)u.y * v.y; };
            auto len = [](CPoint u){ return sqrt((double)u.x*u.x + (double)u.y*u.y); };
            auto normalize = [](double ang){ while (ang < 0) ang += 2*M_PI; while (ang >= 2*M_PI) ang -= 2*M_PI; return ang; };

            CPoint A1, A2;
            if (auto le1 = dynamic_cast<CLineEntity*>(m_pDimEnt1)) {
                A1 = MidPoint(le1->m_ptStart, le1->m_ptEnd);
            } else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
                int idx = max(0, m_nDimSegIndex1);
                A1 = MidPoint(pl->m_vertices[idx], pl->m_vertices[idx+1]);
            } else {
                A1 = center;
            }
            if (auto le2 = dynamic_cast<CLineEntity*>(m_pDimEnt2)) {
                A2 = MidPoint(le2->m_ptStart, le2->m_ptEnd);
            } else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt2Orig)) {
                int idx = max(0, m_nDimSegIndex2);
                A2 = MidPoint(pl->m_vertices[idx], pl->m_vertices[idx+1]);
            } else {
                A2 = center;
            }

            CPoint curWorld = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
            CPoint OA1 = vec(center, A1);
            CPoint OA2 = vec(center, A2);
            CPoint OB = vec(center, curWorld);
            double lenOA1 = len(OA1), lenOA2 = len(OA2), lenOB = len(OB);
            bool isInner = false;
            if (lenOA1 > 1e-6 && lenOA2 > 1e-6 && lenOB > 1e-6) {
                double cos1 = dot(OA1, OB) / (lenOA1 * lenOB);
                double cos2 = dot(OA2, OB) / (lenOA2 * lenOB);
                if (cos1 > 1.0) cos1 = 1.0; if (cos1 < -1.0) cos1 = -1.0;
                if (cos2 > 1.0) cos2 = 1.0; if (cos2 < -1.0) cos2 = -1.0;
                double ang1 = acos(cos1);
                double ang2 = acos(cos2);
                if (ang1 < (M_PI/2.0) && ang2 < (M_PI/2.0)) isInner = true;
            }

            double angA1 = normalize(atan2((double)OA1.y, (double)OA1.x));
            double angA2 = normalize(atan2((double)OA2.y, (double)OA2.x));
            double s1 = angA1, e1 = angA2; double sweep1 = e1 - s1; if (sweep1 < 0) sweep1 += 2*M_PI;
            double s2 = angA2, e2 = angA1; double sweep2 = e2 - s2; if (sweep2 < 0) sweep2 += 2*M_PI;
            double start = 0, end = 0, sweep = 0;
            if (isInner) {
                if (sweep1 <= M_PI) { start = s1; end = e1; sweep = sweep1; }
                else { start = s2; end = e2; sweep = sweep2; }
            } else {
                if (sweep1 > M_PI) { start = s1; end = e1; sweep = sweep1; }
                else { start = s2; end = e2; sweep = sweep2; }
            }

            // compute radius in screen pixels from cursor world distance
            double worldRad = Distance(center, curWorld);
            CLargeHWDoc* pDocLocal = GetDocument();
            int scrR = max(12, (int)floor(worldRad * pDocLocal->m_dScale + 0.5));
            if (scrR < 8) scrR = max(8, (int)(20 * pDocLocal->m_dScale));

            // draw rays and arc in screen coordinates
            CPoint sc = WorldToScreen(center);
            CPoint scrStart((int)floor(sc.x + scrR * cos(start) + 0.5), (int)floor(sc.y + scrR * sin(start) + 0.5));
            CPoint scrEnd((int)floor(sc.x + scrR * cos(end) + 0.5),   (int)floor(sc.y + scrR * sin(end) + 0.5));
            // draw with preview pen and sample the arc (Arc can be affected by ROP modes)
            pDC->MoveTo(sc); pDC->LineTo(scrStart);
            pDC->MoveTo(sc); pDC->LineTo(scrEnd);
            // sample arc into small line segments for reliable preview rendering
            int steps = max(12, (int)ceil(fabs(sweep) / (M_PI/36.0)));
            for (int k = 0; k <= steps; ++k) {
                double ang = start + sweep * (double)k / (double)steps;
                CPoint pp((int)floor(sc.x + scrR * cos(ang) + 0.5), (int)floor(sc.y + scrR * sin(ang) + 0.5));
                if (k == 0) pDC->MoveTo(pp); else pDC->LineTo(pp);
            }
        }
        break;

    case STATE_DRAW_DIM_RADIUS_SELECT:
        if (m_pDimRadiusSrcEnt) {
            CPoint center;
            int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimRadiusSrcEnt)) {
                center = pCirc->m_ptCenter; radius = pCirc->m_nRadius;
            } else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimRadiusSrcEnt)) {
                center = pArc->m_ptCenter; radius = pArc->m_nRadius;
            }
            if (radius > 0) {
                CPoint sc = WorldToScreen(center);
                int r = (int)(radius * pDoc->m_dScale);
                CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                CPen hl(PS_SOLID, 2, RGB(0,255,255));
                CPen* pOld = pDC->SelectObject(&hl);
                pDC->Ellipse(sc.x - r, sc.y - r, sc.x + r, sc.y + r);
                pDC->SelectObject(pOld); pDC->SelectObject(pB);
            }
        }
        break;

    case STATE_DRAW_DIM_RADIUS_POS:
        if (m_pDimRadiusSrcEnt) {
            CPoint center;
            int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimRadiusSrcEnt)) {
                center = pCirc->m_ptCenter; radius = pCirc->m_nRadius;
            } else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimRadiusSrcEnt)) {
                center = pArc->m_ptCenter; radius = pArc->m_nRadius;
            }
            if (radius > 0) {
                CPoint curWorld = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                double dx = (double)(curWorld.x - center.x);
                double dy = (double)(curWorld.y - center.y);
                double dist = sqrt(dx*dx + dy*dy);
                if (dist < 1e-6) dist = 1.0;
                CPoint endPt((int)floor(center.x + radius * dx / dist + 0.5),
                             (int)floor(center.y + radius * dy / dist + 0.5));
                CPoint sc = WorldToScreen(center);
                CPoint se = WorldToScreen(endPt);
                pDC->MoveTo(sc); pDC->LineTo(se);
                double sdx = (double)(se.x - sc.x), sdy = (double)(se.y - sc.y);
                double sd = sqrt(sdx*sdx + sdy*sdy);
                if (sd > 1e-6) {
                    double ux = sdx / sd, uy = sdy / sd;
                    int arrowLen = max(8, (int)(8 * pDoc->m_dScale));
                    double arrowAng = atan2(uy, ux);
                    double a1 = arrowAng + M_PI * 3.0 / 8.0;
                    double a2 = arrowAng - M_PI * 3.0 / 8.0;
                    CPoint p1((int)floor(se.x - cos(a1) * arrowLen + 0.5), (int)floor(se.y - sin(a1) * arrowLen + 0.5));
                    CPoint p2((int)floor(se.x - cos(a2) * arrowLen + 0.5), (int)floor(se.y - sin(a2) * arrowLen + 0.5));
                    pDC->MoveTo(se); pDC->LineTo(p1);
                    pDC->MoveTo(se); pDC->LineTo(p2);
                }
                CString txt; txt.Format(L"%.2f", (double)radius);
                CPoint txtPt = m_bSnapActive ? WorldToScreen(m_ptSnapped) : m_ptCurrent;
                pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
            }
        }
        break;

    case STATE_DRAW_DIM_DIAMETER_SELECT:
        if (m_pDimDiamSrcEnt) {
            CPoint center; int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimDiamSrcEnt)) { center = pCirc->m_ptCenter; radius = pCirc->m_nRadius; }
            else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimDiamSrcEnt)) { center = pArc->m_ptCenter; radius = pArc->m_nRadius; }
            if (radius > 0) {
                CPoint sc = WorldToScreen(center); int r = (int)(radius * pDoc->m_dScale);
                CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                CPen hl(PS_SOLID, 2, RGB(0,255,255)); CPen* pOld = pDC->SelectObject(&hl);
                pDC->Ellipse(sc.x - r, sc.y - r, sc.x + r, sc.y + r);
                pDC->SelectObject(pOld); pDC->SelectObject(pB);
            }
        }
        break;

    case STATE_DRAW_DIM_DIAMETER_POS:
        if (m_pDimDiamSrcEnt) {
            CPoint center; int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimDiamSrcEnt)) { center = pCirc->m_ptCenter; radius = pCirc->m_nRadius; }
            else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimDiamSrcEnt)) { center = pArc->m_ptCenter; radius = pArc->m_nRadius; }
            if (radius > 0) {
                CPoint curWorld = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                double dx = (double)(curWorld.x - center.x), dy = (double)(curWorld.y - center.y);
                double dist = sqrt(dx*dx + dy*dy); if (dist < 1e-6) dist = 1.0;
                CPoint end1((int)floor(center.x - radius * dx / dist + 0.5), (int)floor(center.y - radius * dy / dist + 0.5));
                CPoint end2((int)floor(center.x + radius * dx / dist + 0.5), (int)floor(center.y + radius * dy / dist + 0.5));
                CPoint se1 = WorldToScreen(end1), se2 = WorldToScreen(end2);
                pDC->MoveTo(se1); pDC->LineTo(se2);
                double sdx = (double)(se2.x - se1.x), sdy = (double)(se2.y - se1.y);
                double sd = sqrt(sdx*sdx + sdy*sdy);
                if (sd > 1e-6) {
                    double ux = sdx / sd, uy = sdy / sd; int arrowLen = max(8, (int)(8 * pDoc->m_dScale));
                    double arrowAng = atan2(uy, ux);
                    double a1 = arrowAng + M_PI * 3.0 / 8.0, a2 = arrowAng - M_PI * 3.0 / 8.0;
                    CPoint p1((int)floor(se1.x + cos(a1) * arrowLen + 0.5), (int)floor(se1.y + sin(a1) * arrowLen + 0.5));
                    CPoint p2((int)floor(se1.x + cos(a2) * arrowLen + 0.5), (int)floor(se1.y + sin(a2) * arrowLen + 0.5));
                    pDC->MoveTo(se1); pDC->LineTo(p1); pDC->MoveTo(se1); pDC->LineTo(p2);
                    a1 = arrowAng + M_PI + M_PI * 3.0 / 8.0; a2 = arrowAng + M_PI - M_PI * 3.0 / 8.0;
                    p1 = CPoint((int)floor(se2.x + cos(a1) * arrowLen + 0.5), (int)floor(se2.y + sin(a1) * arrowLen + 0.5));
                    p2 = CPoint((int)floor(se2.x + cos(a2) * arrowLen + 0.5), (int)floor(se2.y + sin(a2) * arrowLen + 0.5));
                    pDC->MoveTo(se2); pDC->LineTo(p1); pDC->MoveTo(se2); pDC->LineTo(p2);
                }
                CString txt; txt.Format(L"%.2f", (double)(radius*2));
                CPoint txtPt = m_bSnapActive ? WorldToScreen(m_ptSnapped) : m_ptCurrent;
                pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
            }
        }
        break;

    case STATE_DRAW_DIM_ARCLEN_SELECT:
        if (m_pDimArcLenSrcEnt) {
            if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimArcLenSrcEnt)) {
                CPoint sc = WorldToScreen(pArc->m_ptCenter);
                int r = (int)(pArc->m_nRadius * pDoc->m_dScale);
                CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                CPen hl(PS_SOLID, 2, RGB(0,255,255)); CPen* pOld = pDC->SelectObject(&hl);
                pDC->Ellipse(sc.x - r, sc.y - r, sc.x + r, sc.y + r);
                pDC->SelectObject(pOld); pDC->SelectObject(pB);
            }
        }
        break;

    case STATE_DRAW_DIM_ARCLEN_POS:
        if (m_pDimArcLenSrcEnt) {
            auto pArc = dynamic_cast<CArcEntity*>(m_pDimArcLenSrcEnt);
            if (!pArc) break;
            CPoint center = pArc->m_ptCenter;
            int arcRadius = pArc->m_nRadius;
            double angStart = atan2((double)(pArc->m_ptStart.y - center.y), (double)(pArc->m_ptStart.x - center.x));
            double angEnd = atan2((double)(pArc->m_ptEnd.y - center.y), (double)(pArc->m_ptEnd.x - center.x));
            double sweep = angEnd - angStart;
            if (sweep < 0) sweep += 2*M_PI;
            double angMid = atan2((double)(pArc->m_ptMid.y - center.y), (double)(pArc->m_ptMid.x - center.x));
            double midSweep = angMid - angStart;
            if (midSweep < 0) midSweep += 2*M_PI;
            if (midSweep > sweep) { double tmp = angStart; angStart = angEnd; angEnd = tmp; sweep = 2*M_PI - sweep; }

            CPoint curWorld = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
            double dist = Distance(center, curWorld);
            if (dist <= arcRadius + 1) {
                CPoint txtPt = m_bSnapActive ? m_ptSnapped : m_ptCurrent;
                pDC->TextOutW(txtPt.x, txtPt.y, L"this position is illegal");
                // Still show extension lines
                CPoint arcS((int)floor(center.x + arcRadius*cos(angStart)+0.5), (int)floor(center.y + arcRadius*sin(angStart)+0.5));
                CPoint arcE((int)floor(center.x + arcRadius*cos(angEnd)+0.5), (int)floor(center.y + arcRadius*sin(angEnd)+0.5));
                CPoint sS = WorldToScreen(arcS), sE = WorldToScreen(arcE);
                pDC->MoveTo(sS); pDC->LineTo(sS.x + (int)((sS.x-WorldToScreen(center).x)*0.3), sS.y + (int)((sS.y-WorldToScreen(center).y)*0.3));
                pDC->MoveTo(sE); pDC->LineTo(sE.x + (int)((sE.x-WorldToScreen(center).x)*0.3), sE.y + (int)((sE.y-WorldToScreen(center).y)*0.3));
                break;
            }

            // Compute dimension arc endpoints
            CPoint dimS((int)floor(center.x + dist*cos(angStart)+0.5), (int)floor(center.y + dist*sin(angStart)+0.5));
            CPoint dimE((int)floor(center.x + dist*cos(angEnd)+0.5), (int)floor(center.y + dist*sin(angEnd)+0.5));
            CPoint arcS((int)floor(center.x + arcRadius*cos(angStart)+0.5), (int)floor(center.y + arcRadius*sin(angStart)+0.5));
            CPoint arcE((int)floor(center.x + arcRadius*cos(angEnd)+0.5), (int)floor(center.y + arcRadius*sin(angEnd)+0.5));
            CPoint sc = WorldToScreen(center), sDS = WorldToScreen(dimS), sDE = WorldToScreen(dimE);
            CPoint sAS = WorldToScreen(arcS), sAE = WorldToScreen(arcE);
            int sdr = (int)(dist * pDoc->m_dScale);

            // Extension lines
            pDC->MoveTo(sAS); pDC->LineTo(sDS);
            pDC->MoveTo(sAE); pDC->LineTo(sDE);

            // Dimension arc - use Arc() for exact match with final rendering
            CRect rcE(sc.x - sdr, sc.y - sdr, sc.x + sdr, sc.y + sdr);
            pDC->Arc(rcE, sDS, sDE);

            // Arrowheads at arc ends
            int arrowLen = max(8, (int)(8 * pDoc->m_dScale));
            double sweepDir = sweep > 0 ? M_PI/2.0 : -M_PI/2.0;
            auto drawArrow = [&](CPoint pt, double angle, bool isStart) {
                double tanAng = angle + (isStart ? sweepDir - M_PI : sweepDir);
                double a1 = tanAng + M_PI * 3.0 / 8.0, a2 = tanAng - M_PI * 3.0 / 8.0;
                CPoint p1((int)floor(pt.x + cos(a1)*arrowLen + 0.5), (int)floor(pt.y + sin(a1)*arrowLen + 0.5));
                CPoint p2((int)floor(pt.x + cos(a2)*arrowLen + 0.5), (int)floor(pt.y + sin(a2)*arrowLen + 0.5));
                pDC->MoveTo(pt); pDC->LineTo(p1); pDC->MoveTo(pt); pDC->LineTo(p2);
            };
            drawArrow(sDS, angStart, true);
            drawArrow(sDE, angEnd, false);

            // Text preview
            double arcLen = (double)arcRadius * fabs(sweep);
            CString txt; txt.Format(L"%.2f", arcLen);
            CPoint txtPt = m_bSnapActive ? m_ptSnapped : m_ptCurrent;
            pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
        }
        break;

    case STATE_DRAW_TEXT_POS:
        if (m_pPendingDim) {
            if (auto pL = dynamic_cast<CDimLinearEntity*>(m_pPendingDim)) {
                CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                CPoint mid = WorldToScreen(place);
                double lenVal;
                if (pL->m_mode == CDimLinearEntity::DIM_HORIZONTAL) lenVal = fabs((double)(pL->m_pt2.x - pL->m_pt1.x));
                else if (pL->m_mode == CDimLinearEntity::DIM_VERTICAL) lenVal = fabs((double)(pL->m_pt2.y - pL->m_pt1.y));
                else lenVal = Distance(pL->m_pt1, pL->m_pt2);
                CString txt; txt.Format(L"%.2f", lenVal);
                pDC->TextOutW(mid.x+4, mid.y+4, txt);
            } else if (auto pA = dynamic_cast<CDimAngularEntity*>(m_pPendingDim)) {
                CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                double a1 = pA->m_ang1; double a2 = pA->m_ang2;
                auto norm = [](double ang){ while (ang < 0) ang += 2*M_PI; while (ang >= 2*M_PI) ang -= 2*M_PI; return ang; };
                double start = norm(a1), end = norm(a2);
                double sweep = end - start; if (sweep < 0) sweep += 2*M_PI;
                double angle = sweep; if (angle > 2*M_PI) angle = fmod(angle, 2*M_PI);
                double deg = angle * 180.0 / M_PI;
                CString txt; txt.Format(L"%.1f deg", deg);

                CPoint txtPt = WorldToScreen(place);
                pDC->TextOutW(txtPt.x, txtPt.y, txt);
            } else if (auto pR = dynamic_cast<CDimRadiusEntity*>(m_pPendingDim)) {
                CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                CString txt; txt.Format(L"%.2f", (double)pR->m_nRadius);
                CPoint txtPt = WorldToScreen(place);
                pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
            } else if (auto pD = dynamic_cast<CDimDiamEntity*>(m_pPendingDim)) {
                CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                CString txt; txt.Format(L"%.2f", (double)(pD->m_nRadius*2));
                CPoint txtPt = WorldToScreen(place);
                pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
            } else if (auto pAL = dynamic_cast<CDimArcLengthEntity*>(m_pPendingDim)) {
                CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                double arcLen = (double)pAL->m_nArcRadius * fabs(pAL->m_dSweep);
                CString txt; txt.Format(L"%.2f", arcLen);
                CPoint txtPt = WorldToScreen(place);
                pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
            }
        }
        if (m_bCoordDimMode && m_tempPts.size() >= 1) {
            CPoint screenPt = WorldToScreen(m_ptCoordPoint);
            int crossLen = 4;
            pDC->MoveTo(screenPt.x - crossLen, screenPt.y);
            pDC->LineTo(screenPt.x + crossLen, screenPt.y);
            pDC->MoveTo(screenPt.x, screenPt.y - crossLen);
            pDC->LineTo(screenPt.x, screenPt.y + crossLen);
            CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
            CString txt; txt.Format(L"(%.2f, %.2f)", (double)m_ptCoordPoint.x, (double)m_ptCoordPoint.y);
            CPoint txtPt = WorldToScreen(place);
            pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
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

    // dimension preview handled above; interactive creation occurs in OnLButtonDown

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
    case STATE_DRAW_DIM_LENGTH_P1:   pDoc->m_strCommandPrompt = L"DIMENSION Specify first extension point: "; break;
    case STATE_DRAW_DIM_LENGTH_P2:   pDoc->m_strCommandPrompt = L"DIMENSION Specify second extension point: "; break;
    case STATE_DRAW_DIM_LENGTH_POS:  pDoc->m_strCommandPrompt = L"DIMENSION Click to place dimension line and text: "; break;
    case STATE_DRAW_DIM_ANGLE_SELECT_E1: pDoc->m_strCommandPrompt = L"ANGDIM Select first line: "; break;
    case STATE_DRAW_DIM_ANGLE_SELECT_E2: pDoc->m_strCommandPrompt = L"ANGDIM Select second line: "; break;
    case STATE_DRAW_DIM_ANGLE_POS:       pDoc->m_strCommandPrompt = L"ANGDIM Pick position on arc for dimension: "; break;
    case STATE_DRAW_DIM_RADIUS_SELECT:   pDoc->m_strCommandPrompt = L"RADDIM Select circle or arc: "; break;
    case STATE_DRAW_DIM_RADIUS_POS:      pDoc->m_strCommandPrompt = L"RADDIM Specify leader line position: "; break;
    case STATE_DRAW_DIM_DIAMETER_SELECT: pDoc->m_strCommandPrompt = L"DIADIM Select circle or arc: "; break;
    case STATE_DRAW_DIM_DIAMETER_POS:    pDoc->m_strCommandPrompt = L"DIADIM Specify diameter line position: "; break;
    case STATE_DRAW_DIM_ARCLEN_SELECT:   pDoc->m_strCommandPrompt = L"ARCLENDIM Select arc: "; break;
    case STATE_DRAW_DIM_ARCLEN_POS:      pDoc->m_strCommandPrompt = L"ARCLENDIM Specify dimension arc position: "; break;
    case STATE_DRAW_DIM_COORD_PICK:      pDoc->m_strCommandPrompt = L"COORD Specify point: "; break;
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
            // Ctrl+click on empty space → zoom window drag
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
        // If we have a pending dimension entity, use this click as text placement
        if (m_pPendingDim) {
            if (auto pL = dynamic_cast<CDimLinearEntity*>(m_pPendingDim)) {
                pL->m_ptText = world;
                pL->m_bTextPlaced = true;
                pL->m_bSelected = false;
                m_pPendingDim = nullptr;
                CompleteDrawCommand();
            } else if (auto pA = dynamic_cast<CDimAngularEntity*>(m_pPendingDim)) {
                // set text position
                pA->m_ptText = world;
                pA->m_bTextPlaced = true;
                pA->m_bSelected = false;
                // restore any temporary splits created during angle selection
                if (!m_tempSplitNewIDs.empty()) {
                    RestoreTemporarySplits(pDoc, m_tempSplitNewIDs);
                    m_tempSplitNewIDs.clear();
                }
                // clear selection so any previously highlighted (thick) entities return to normal
                pDoc->DeselectAll();
                // cleanup temp segment entities if created
                if (m_bDimEnt1Temp && m_pDimEnt1) delete m_pDimEnt1;
                if (m_bDimEnt2Temp && m_pDimEnt2) delete m_pDimEnt2;
                m_pDimEnt1 = m_pDimEnt2 = nullptr;
                m_pDimEnt1Orig = m_pDimEnt2Orig = nullptr;
                m_bDimEnt1Temp = m_bDimEnt2Temp = false;
                m_pPendingDim = nullptr;
                CompleteDrawCommand();
            } else if (auto pR = dynamic_cast<CDimRadiusEntity*>(m_pPendingDim)) {
                pR->m_ptText = world;
                pR->m_bTextPlaced = true;
                pR->m_bSelected = false;
                m_pPendingDim = nullptr;
                m_pDimRadiusSrcEnt = nullptr;
                pDoc->DeselectAll();
                CompleteDrawCommand();
            } else if (auto pD = dynamic_cast<CDimDiamEntity*>(m_pPendingDim)) {
                pD->m_ptText = world;
                pD->m_bTextPlaced = true;
                pD->m_bSelected = false;
                m_pPendingDim = nullptr;
                m_pDimDiamSrcEnt = nullptr;
                pDoc->DeselectAll();
                CompleteDrawCommand();
            } else if (auto pAL = dynamic_cast<CDimArcLengthEntity*>(m_pPendingDim)) {
                pAL->m_ptText = world;
                pAL->m_bTextPlaced = true;
                pAL->m_bSelected = false;
                m_pPendingDim = nullptr;
                m_pDimArcLenSrcEnt = nullptr;
                pDoc->DeselectAll();
                CompleteDrawCommand();
            }
        } else if (m_bCoordDimMode) {
            CString txt;
            txt.Format(L"(%.2f, %.2f)", (double)m_ptCoordPoint.x, (double)m_ptCoordPoint.y);
            CTextEntity* pText = new CTextEntity(world, txt, 20);
            pDoc->AddEntity(pText);
            // Draw cross marker at the coordinate point (4 screen pixels)
            double halfW = (pDoc->m_dScale > 0.0) ? (2.0 / pDoc->m_dScale) : 2.0;
            CLineEntity* pLineH = new CLineEntity(
                CPoint((int)(m_ptCoordPoint.x - halfW + 0.5), m_ptCoordPoint.y),
                CPoint((int)(m_ptCoordPoint.x + halfW + 0.5), m_ptCoordPoint.y));
            CLineEntity* pLineV = new CLineEntity(
                CPoint(m_ptCoordPoint.x, (int)(m_ptCoordPoint.y - halfW + 0.5)),
                CPoint(m_ptCoordPoint.x, (int)(m_ptCoordPoint.y + halfW + 0.5)));
            pDoc->AddEntity(pLineH);
            pDoc->AddEntity(pLineV);
            m_bCoordDimMode = false;
            CompleteDrawCommand();
        } else {
            // fallback: normal text input dialog
            CTextInputDlg dlg(this);
            if (dlg.DoModal() == IDOK) {
                CTextEntity* pText = new CTextEntity(world, dlg.m_strText, dlg.m_nHeight);
                pDoc->AddEntity(pText);
            }
            CompleteDrawCommand();
        }
        break;
    }

    case STATE_DRAW_DIM_LENGTH_P1:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_DIM_LENGTH_P2);
        break;

    case STATE_DRAW_DIM_LENGTH_P2: {
        // push second point and move to placement state so user can click to place offset
        m_tempPts.push_back(world);
        // reserve a third temp slot to hold offset world point chosen by user (initially compute default offset)
        CPoint a = m_tempPts[0], b = m_tempPts[1];
        double wx = (double)(b.x - a.x), wy = (double)(b.y - a.y);
        double wlen = sqrt(wx*wx + wy*wy);
        double screenOffset = 15.0; double offWorld = 1.0; CLargeHWDoc* pDocLocal = GetDocument();
        if (pDocLocal && pDocLocal->m_dScale > 0.0) offWorld = screenOffset / pDocLocal->m_dScale;
        CPoint offsetPt = a;
        if (wlen > 1e-6) {
            double ux = wx / wlen, uy = wy / wlen;
            double nx = -uy, ny = ux;
            offsetPt.x = (int)floor(a.x + nx * offWorld + 0.5);
            offsetPt.y = (int)floor(a.y + ny * offWorld + 0.5);
        } else {
            offsetPt.y = a.y + (int)offWorld;
        }
        m_tempPts.push_back(offsetPt);
        SetDrawState(STATE_DRAW_DIM_LENGTH_POS);
        break;
    }

    case STATE_DRAW_DIM_LENGTH_POS: {
        // First click: create the dimension graphics (without text placement). Next click will place the text.
        if (m_tempPts.size() >= 3) {
            CPoint a = m_tempPts[0], b = m_tempPts[1];
            CPoint place = world; // click position
            // compute signed offset along normal from original line to placement point (in world units)
            double wx = (double)(b.x - a.x), wy = (double)(b.y - a.y);
            double wlen = sqrt(wx*wx + wy*wy);
            double signedOff = 0.0;
            if (wlen > 1e-6) {
                double nx = -(wy / wlen), ny = (wx / wlen);
                double pax = (double)(place.x - a.x), pay = (double)(place.y - a.y);
                signedOff = pax * nx + pay * ny; // projection onto normal
            } else {
                signedOff = (double)(place.y - a.y);
            }
            CDimLinearEntity::DimMode mode = CDimLinearEntity::DIM_ALIGNED;
            if (m_nLastDimMode == 1) mode = CDimLinearEntity::DIM_HORIZONTAL;
            else if (m_nLastDimMode == 2) mode = CDimLinearEntity::DIM_VERTICAL;
            CDimLinearEntity* pDim = new CDimLinearEntity(a, b, mode, signedOff, place);
            pDim->m_bTextPlaced = false;
            // add to document so graphics appear immediately, keep as pending for text placement
            pDoc->AddEntity(pDim);
            pDim->m_bSelected = true;
            if (m_pPendingDim && m_pPendingDim != pDim) m_pPendingDim = nullptr;
            m_pPendingDim = pDim;
            // move to a dedicated text placement state so next click places text
            SetDrawState(STATE_DRAW_TEXT_POS);
        } else {
            CompleteDrawCommand();
        }
        break;
    }



    case STATE_DRAW_DIM_ANGLE_SELECT_E1: {
        // When starting angle dimension selection, create temporary splits so subsequent
        // selections operate on split segments. The temporary splits will be restored later.
        m_tempSplitNewIDs.clear();
        CreateTemporarySplits(pDoc, m_tempSplitNewIDs);
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_DRAW_DIM_ANGLE_SELECT_E1: hit=%p\n", hit);
        if (hit) {
            // If it's a polyline, determine which segment was clicked and create a temp CLineEntity
            m_pDimEnt1Orig = hit;
            m_pDimEnt1 = hit;
            m_bDimEnt1Temp = false;
            m_nDimSegIndex1 = -1;
            if (auto pl = dynamic_cast<CPolylineEntity*>(hit)) {
                // find nearest segment
                double best = 1e9; int bestIdx = -1;
                CPoint sp = point;
                for (int i = 0; i < pl->GetVertexCount() - 1; ++i) {
                    CPoint p1 = WorldToScreen(pl->m_vertices[i]);
                    CPoint p2 = WorldToScreen(pl->m_vertices[i+1]);
                    double d = PointToLineDistance(sp, p1, p2);
                    if (d < best) { best = d; bestIdx = i; }
                }
                if (bestIdx >= 0) {
                    // always record which segment is nearest for highlighting
                    m_nDimSegIndex1 = bestIdx;
                }
                if (bestIdx >= 0 && best <= 8.0) {
                    // try to find an existing temporary split segment created at command start
                    CPoint sPt = pl->m_vertices[bestIdx];
                    CPoint ePt = pl->m_vertices[bestIdx+1];
                    CEntity* found = nullptr;
                    for (int id : m_tempSplitNewIDs) {
                        CEntity* ee = pDoc->FindEntityByID(id);
                        if (!ee) continue;
                        if (auto lne = dynamic_cast<CLineEntity*>(ee)) {
                            if ((lne->m_ptStart == sPt && lne->m_ptEnd == ePt) ||
                                (lne->m_ptStart == ePt && lne->m_ptEnd == sPt)) {
                                found = ee; break;
                            }
                        }
                    }
                    if (found) {
                        m_pDimEnt1 = found; m_bDimEnt1Temp = false; m_nDimSegIndex1 = bestIdx;
                    } else {
                        // not found among pre-created splits -> create and add as temporary doc entity
                        CLineEntity* ln = new CLineEntity(sPt, ePt);
                        pDoc->AddEntity(ln);
                        m_tempSplitNewIDs.push_back(ln->m_nID);
                        m_pDimEnt1 = ln; m_bDimEnt1Temp = false; m_nDimSegIndex1 = bestIdx;
                    }
                }
            }
            pDoc->DeselectAll();
            m_pDimEnt1->m_bSelected = true;
            Invalidate(FALSE);
            SetDrawState(STATE_DRAW_DIM_ANGLE_SELECT_E2);
        }
        break;
    }

    case STATE_DRAW_DIM_ANGLE_SELECT_E2: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_DRAW_DIM_ANGLE_SELECT_E2: hit=%p\n", hit);
        if (hit && hit != m_pDimEnt1) {
            m_pDimEnt2Orig = hit;
            m_pDimEnt2 = hit;
            m_bDimEnt2Temp = false;
            m_nDimSegIndex2 = -1;
            if (auto pl = dynamic_cast<CPolylineEntity*>(hit)) {
                double best = 1e9; int bestIdx = -1;
                CPoint sp = point;
                for (int i = 0; i < pl->GetVertexCount() - 1; ++i) {
                    CPoint p1 = WorldToScreen(pl->m_vertices[i]);
                    CPoint p2 = WorldToScreen(pl->m_vertices[i+1]);
                    double d = PointToLineDistance(sp, p1, p2);
                    if (d < best) { best = d; bestIdx = i; }
                }
                if (bestIdx >= 0) {
                    m_nDimSegIndex2 = bestIdx;
                }
                if (bestIdx >= 0 && best <= 8.0) {
                    // try to reuse existing temporary split segment if present
                    CPoint sPt = pl->m_vertices[bestIdx];
                    CPoint ePt = pl->m_vertices[bestIdx+1];
                    CEntity* found = nullptr;
                    for (int id : m_tempSplitNewIDs) {
                        CEntity* ee = pDoc->FindEntityByID(id);
                        if (!ee) continue;
                        if (auto lne = dynamic_cast<CLineEntity*>(ee)) {
                            if ((lne->m_ptStart == sPt && lne->m_ptEnd == ePt) ||
                                (lne->m_ptStart == ePt && lne->m_ptEnd == sPt)) {
                                found = ee; break;
                            }
                        }
                    }
                    if (found) {
                        m_pDimEnt2 = found; m_bDimEnt2Temp = false; m_nDimSegIndex2 = bestIdx;
                    } else {
                        // not found among pre-created splits -> create and add as temporary doc entity
                        CLineEntity* ln = new CLineEntity(sPt, ePt);
                        pDoc->AddEntity(ln);
                        m_tempSplitNewIDs.push_back(ln->m_nID);
                        m_pDimEnt2 = ln; m_bDimEnt2Temp = false; m_nDimSegIndex2 = bestIdx;
                    }
                }
            }
            pDoc->DeselectAll();
            m_pDimEnt2->m_bSelected = true;
            Invalidate(FALSE);
            pDoc->m_strCommandPrompt = L"Pick position on arc for dimension (inside/outside): ";
            TRACE(L"[DEBUG] OnLButtonDown: entering STATE_DRAW_DIM_ANGLE_POS, pDim1=%p pDim2=%p\n", m_pDimEnt1, m_pDimEnt2);
            SetDrawState(STATE_DRAW_DIM_ANGLE_POS);
        }
        break;
    }

    case STATE_DRAW_DIM_ANGLE_POS: {
        // The user clicked a point that indicates where the arc should be drawn (chooses inner/outer)
        // We'll compute two direction vectors from the intersection/center and create the CDimAngularEntity
        if (!m_pDimEnt1 || !m_pDimEnt2) { CompleteDrawCommand(); break; }

        // Determine reference points on each entity nearest to clicked point
        CPoint pA, pB, center;
        // For simplicity, if both are lines, use their intersection as center; otherwise use first entity center
        CLineEntity* l1 = dynamic_cast<CLineEntity*>(m_pDimEnt1);
        CLineEntity* l2 = dynamic_cast<CLineEntity*>(m_pDimEnt2);
        if (l1 && l2) {
            // compute intersection of two lines (in world coords)
            CPoint a1 = l1->m_ptStart, a2 = l1->m_ptEnd;
            CPoint b1 = l2->m_ptStart, b2 = l2->m_ptEnd;
            double x1 = a1.x, y1 = a1.y, x2 = a2.x, y2 = a2.y;
            double x3 = b1.x, y3 = b1.y, x4 = b2.x, y4 = b2.y;
            double den = (x1 - x2)*(y3 - y4) - (y1 - y2)*(x3 - x4);
            if (fabs(den) < 1e-6) {
                // parallel -> fallback to midpoints
                center = MidPoint(a1, a2);
            } else {
                double px = ((x1*y2 - y1*x2)*(x3 - x4) - (x1 - x2)*(x3*y4 - y3*x4)) / den;
                double py = ((x1*y2 - y1*x2)*(y3 - y4) - (y1 - y2)*(x3*y4 - y3*x4)) / den;
                center = CPoint((int)px, (int)py);
            }
            // pick directions as nearest points on each line from center
            pA = a1; pB = b1;
        } else {
            // fallback: pick centers or midpoints
            if (l1) { center = MidPoint(l1->m_ptStart, l1->m_ptEnd); pA = l1->m_ptStart; }
            else if (l2) { center = MidPoint(l2->m_ptStart, l2->m_ptEnd); pB = l2->m_ptStart; }
            else { CompleteDrawCommand(); break; }
        }

        // Construct angular dim using clicked point as arc placement reference
        // compute angles: use direction from center toward nearest points on each selected segment/line
        double clickedA = atan2((double)(world.y - center.y), (double)(world.x - center.x));
        double a1 = 0, a2 = 0;
        if (auto l1p = dynamic_cast<CLineEntity*>(m_pDimEnt1)) {
            CPoint cp = ClosestPointOnSegment(l1p->m_ptStart, l1p->m_ptEnd, center);
            a1 = atan2((double)(cp.y - center.y), (double)(cp.x - center.x));
        } else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
            int idx = (m_nDimSegIndex1 >= 0) ? m_nDimSegIndex1 : 0;
            CPoint s = pl->m_vertices[idx], e = pl->m_vertices[idx+1];
            CPoint cp = ClosestPointOnSegment(s, e, center);
            a1 = atan2((double)(cp.y - center.y), (double)(cp.x - center.x));
        } else {
            a1 = atan2((double)(pA.y - center.y), (double)(pA.x - center.x));
        }
        if (auto l2p = dynamic_cast<CLineEntity*>(m_pDimEnt2)) {
            CPoint cp = ClosestPointOnSegment(l2p->m_ptStart, l2p->m_ptEnd, center);
            a2 = atan2((double)(cp.y - center.y), (double)(cp.x - center.x));
        } else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt2Orig)) {
            int idx = (m_nDimSegIndex2 >= 0) ? m_nDimSegIndex2 : 0;
            CPoint s = pl->m_vertices[idx], e = pl->m_vertices[idx+1];
            CPoint cp = ClosestPointOnSegment(s, e, center);
            a2 = atan2((double)(cp.y - center.y), (double)(cp.x - center.x));
        } else {
            a2 = atan2((double)(pB.y - center.y), (double)(pB.x - center.x));
        }
        // Use OA1/OA2/OB method suggested: compute midpoints A1/A2 for each selected segment
        auto vec = [](CPoint a, CPoint b){ return CPoint(b.x - a.x, b.y - a.y); };
        auto dot = [](CPoint u, CPoint v){ return (double)u.x * v.x + (double)u.y * v.y; };
        auto len = [](CPoint u){ return sqrt((double)u.x*u.x + (double)u.y*u.y); };

        CPoint A1, A2;
        if (auto l1p2 = dynamic_cast<CLineEntity*>(m_pDimEnt1)) A1 = MidPoint(l1p2->m_ptStart, l1p2->m_ptEnd);
        else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
            int idx = (m_nDimSegIndex1 >= 0) ? m_nDimSegIndex1 : 0;
            A1 = MidPoint(pl->m_vertices[idx], pl->m_vertices[idx+1]);
        } else A1 = pA;
        if (auto l2p2 = dynamic_cast<CLineEntity*>(m_pDimEnt2)) A2 = MidPoint(l2p2->m_ptStart, l2p2->m_ptEnd);
        else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt2Orig)) {
            int idx = (m_nDimSegIndex2 >= 0) ? m_nDimSegIndex2 : 0;
            A2 = MidPoint(pl->m_vertices[idx], pl->m_vertices[idx+1]);
        } else A2 = pB;

        CPoint OA1 = vec(center, A1);
        CPoint OA2 = vec(center, A2);
        CPoint OB  = vec(center, world);
        double lenOA1 = len(OA1), lenOA2 = len(OA2), lenOB = len(OB);
        bool isInner = false;
        if (lenOA1 > 1e-6 && lenOA2 > 1e-6 && lenOB > 1e-6) {
            double cos1 = dot(OA1, OB) / (lenOA1 * lenOB);
            double cos2 = dot(OA2, OB) / (lenOA2 * lenOB);
            if (cos1 > 1.0) cos1 = 1.0; if (cos1 < -1.0) cos1 = -1.0;
            if (cos2 > 1.0) cos2 = 1.0; if (cos2 < -1.0) cos2 = -1.0;
            double ang1 = acos(cos1);
            double ang2 = acos(cos2);
            // if both angles < 90deg -> inner
            if (ang1 < (M_PI/2.0) && ang2 < (M_PI/2.0)) isInner = true;
        }

        // compute angular positions for A1/A2
        double angA1 = atan2((double)OA1.y, (double)OA1.x);
        double angA2 = atan2((double)OA2.y, (double)OA2.x);
        auto normalize = [](double ang){ while (ang < 0) ang += 2*M_PI; while (ang >= 2*M_PI) ang -= 2*M_PI; return ang; };
        angA1 = normalize(angA1); angA2 = normalize(angA2);

        // determine explicit candidate arcs and pick according to isInner
        double s1 = angA1, e1 = angA2; double sweep1 = e1 - s1; if (sweep1 < 0) sweep1 += 2*M_PI;
        double s2 = angA2, e2 = angA1; double sweep2 = e2 - s2; if (sweep2 < 0) sweep2 += 2*M_PI;
        double start, end;
        if (isInner) {
            // inner -> pick the smaller (minor) sweep
            if (sweep1 <= M_PI) { start = s1; end = e1; } else { start = s2; end = e2; }
        } else {
            // outer -> pick the larger (major) sweep
            if (sweep1 > M_PI) { start = s1; end = e1; } else { start = s2; end = e2; }
        }

        // compute safe radius based on nearest points on the two segments so arc stays between rays
        double d1 = 10000.0, d2 = 10000.0;
        if (auto l1p2 = dynamic_cast<CLineEntity*>(m_pDimEnt1)) {
            CPoint cp = ClosestPointOnSegment(l1p2->m_ptStart, l1p2->m_ptEnd, center);
            d1 = Distance(center, cp);
        } else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
            int idx = (m_nDimSegIndex1 >= 0) ? m_nDimSegIndex1 : 0;
            CPoint s = pl->m_vertices[idx], e = pl->m_vertices[idx+1];
            CPoint cp = ClosestPointOnSegment(s, e, center);
            d1 = Distance(center, cp);
        }
        if (auto l2p2 = dynamic_cast<CLineEntity*>(m_pDimEnt2)) {
            CPoint cp = ClosestPointOnSegment(l2p2->m_ptStart, l2p2->m_ptEnd, center);
            d2 = Distance(center, cp);
        } else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt2Orig)) {
            int idx = (m_nDimSegIndex2 >= 0) ? m_nDimSegIndex2 : 0;
            CPoint s = pl->m_vertices[idx], e = pl->m_vertices[idx+1];
            CPoint cp = ClosestPointOnSegment(s, e, center);
            d2 = Distance(center, cp);
        }
        double rad = min(d1, d2);
        if (rad < 8.0) rad = max(8.0, min(20.0, max(d1, d2)));
        int r = (int)rad;
        // Before creating the angular dimension, if the two selected entities are line segments,
        // split them at their intersection point and add the split pieces as new line entities.
        CLineEntity* line1 = dynamic_cast<CLineEntity*>(m_pDimEnt1);
        CLineEntity* line2 = dynamic_cast<CLineEntity*>(m_pDimEnt2);
        std::vector<int> createdIDs;
        if (line1 && line2) {
            double x1 = line1->m_ptStart.x, y1 = line1->m_ptStart.y;
            double x2 = line1->m_ptEnd.x,   y2 = line1->m_ptEnd.y;
            double x3 = line2->m_ptStart.x, y3 = line2->m_ptStart.y;
            double x4 = line2->m_ptEnd.x,   y4 = line2->m_ptEnd.y;
            double den = (x1 - x2)*(y3 - y4) - (y1 - y2)*(x3 - x4);
            if (fabs(den) > 1e-6) {
                double px = ((x1*y2 - y1*x2)*(x3 - x4) - (x1 - x2)*(x3*y4 - y3*x4)) / den;
                double py = ((x1*y2 - y1*x2)*(y3 - y4) - (y1 - y2)*(x3*y4 - y3*x4)) / den;
                CPoint ip((int)px, (int)py);
                // check intersection lies within both segments (with small tolerance)
                auto within = [](CPoint a, CPoint b, CPoint p){
                    int minx = min(a.x, b.x) - 1; int maxx = max(a.x, b.x) + 1;
                    int miny = min(a.y, b.y) - 1; int maxy = max(a.y, b.y) + 1;
                    return (p.x >= minx && p.x <= maxx && p.y >= miny && p.y <= maxy);
                };
                if (within(line1->m_ptStart, line1->m_ptEnd, ip) && within(line2->m_ptStart, line2->m_ptEnd, ip)) {
                    // create segments A1-O and O-A2 if non-zero length
                    if (Distance(line1->m_ptStart, ip) > 1e-6) {
                        CLineEntity* a = new CLineEntity(line1->m_ptStart, ip);
                        pDoc->AddEntity(a);
                        createdIDs.push_back(a->m_nID);
                    }
                    if (Distance(ip, line1->m_ptEnd) > 1e-6) {
                        CLineEntity* b = new CLineEntity(ip, line1->m_ptEnd);
                        pDoc->AddEntity(b);
                        createdIDs.push_back(b->m_nID);
                    }
                    if (Distance(line2->m_ptStart, ip) > 1e-6) {
                        CLineEntity* c = new CLineEntity(line2->m_ptStart, ip);
                        pDoc->AddEntity(c);
                        createdIDs.push_back(c->m_nID);
                    }
                    if (Distance(ip, line2->m_ptEnd) > 1e-6) {
                        CLineEntity* d = new CLineEntity(ip, line2->m_ptEnd);
                        pDoc->AddEntity(d);
                        createdIDs.push_back(d->m_nID);
                    }
                    // deselect originals and select created pieces for highlighting
                    pDoc->DeselectAll();
                    for (int id : createdIDs) {
                        for (auto* pe : pDoc->GetEntities()) if (pe->m_nID == id) pe->m_bSelected = true;
                    }
                }
            }
        }

        CPoint rp1((int)(center.x + r * cos(start)), (int)(center.y + r * sin(start)));
        CPoint rp2((int)(center.x + r * cos(end)),   (int)(center.y + r * sin(end)));
        // use 'world' (user click) as placement point and pass exact start/end angles
        CDimAngularEntity* pDimA = new CDimAngularEntity(center, rp1, rp2, world, start, end);
        pDimA->m_bTextPlaced = false;
        // add to document so it's visible; keep reference as pending for text placement
        pDoc->AddEntity(pDimA);
        pDimA->m_bSelected = true;
        if (m_pPendingDim && m_pPendingDim != pDimA) m_pPendingDim = nullptr;
        m_pPendingDim = pDimA;
        // prompt for text placement and keep temporary splits until text is placed
        pDoc->m_strCommandPrompt = L"Place dimension text: ";
        SetDrawState(STATE_DRAW_TEXT_POS);
        break;
    }

    case STATE_DRAW_DIM_RADIUS_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hit) {
            if (dynamic_cast<CCircleEntity*>(hit) || dynamic_cast<CArcEntity*>(hit)) {
                m_pDimRadiusSrcEnt = hit;
                pDoc->DeselectAll();
                hit->m_bSelected = true;
                Invalidate(FALSE);
                pDoc->m_strCommandPrompt = L"Specify leader line position: ";
                SetDrawState(STATE_DRAW_DIM_RADIUS_POS);
            } else {
                pDoc->m_strCommandPrompt = L"Not a circle or arc. Select circle or arc: ";
                CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
                if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
                    pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
                    int nLen = pDoc->m_strCommandPrompt.GetLength();
                    pFrame->m_wndCmdLine.SetSel(nLen, nLen);
                }
            }
        }
        break;
    }

    case STATE_DRAW_DIM_RADIUS_POS: {
        if (m_pDimRadiusSrcEnt) {
            CPoint center;
            int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimRadiusSrcEnt)) {
                center = pCirc->m_ptCenter; radius = pCirc->m_nRadius;
            } else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimRadiusSrcEnt)) {
                center = pArc->m_ptCenter; radius = pArc->m_nRadius;
            }
            if (radius > 0) {
                double dx = (double)(world.x - center.x);
                double dy = (double)(world.y - center.y);
                double dist = sqrt(dx*dx + dy*dy);
                if (dist < 1e-6) dist = 1.0;
                double angle = atan2(dy, dx);
                CDimRadiusEntity* pDim = new CDimRadiusEntity(center, radius, angle);
                pDim->m_bTextPlaced = false;
                pDim->m_color = m_currentColor;
                pDim->m_nLineStyle = m_currentLineStyle;
                pDim->m_nLineWidth = m_currentLineWidth;
                pDoc->AddEntity(pDim);
                pDim->m_bSelected = true;
                m_pPendingDim = pDim;
                m_pDimRadiusSrcEnt = nullptr;
                pDoc->m_strCommandPrompt = L"Place dimension text: ";
                SetDrawState(STATE_DRAW_TEXT_POS);
            }
        } else {
            CompleteDrawCommand();
        }
        break;
    }

    case STATE_DRAW_DIM_DIAMETER_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hit) {
            if (dynamic_cast<CCircleEntity*>(hit) || dynamic_cast<CArcEntity*>(hit)) {
                m_pDimDiamSrcEnt = hit;
                pDoc->DeselectAll();
                hit->m_bSelected = true;
                Invalidate(FALSE);
                pDoc->m_strCommandPrompt = L"Specify diameter line position: ";
                SetDrawState(STATE_DRAW_DIM_DIAMETER_POS);
            } else {
                pDoc->m_strCommandPrompt = L"Not a circle or arc. Select circle or arc: ";
                CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
                if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
                    pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
                    int nLen = pDoc->m_strCommandPrompt.GetLength();
                    pFrame->m_wndCmdLine.SetSel(nLen, nLen);
                }
            }
        }
        break;
    }

    case STATE_DRAW_DIM_DIAMETER_POS: {
        if (m_pDimDiamSrcEnt) {
            CPoint center; int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimDiamSrcEnt)) { center = pCirc->m_ptCenter; radius = pCirc->m_nRadius; }
            else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimDiamSrcEnt)) { center = pArc->m_ptCenter; radius = pArc->m_nRadius; }
            if (radius > 0) {
                double dx = (double)(world.x - center.x), dy = (double)(world.y - center.y);
                double dist = sqrt(dx*dx + dy*dy); if (dist < 1e-6) dist = 1.0;
                double angle = atan2(dy, dx);
                CDimDiamEntity* pDim = new CDimDiamEntity(center, radius, angle);
                pDim->m_bTextPlaced = false;
                pDim->m_color = m_currentColor;
                pDim->m_nLineStyle = m_currentLineStyle;
                pDim->m_nLineWidth = m_currentLineWidth;
                pDoc->AddEntity(pDim);
                pDim->m_bSelected = true;
                m_pPendingDim = pDim;
                m_pDimDiamSrcEnt = nullptr;
                pDoc->m_strCommandPrompt = L"Place dimension text: ";
                SetDrawState(STATE_DRAW_TEXT_POS);
            }
        } else {
            CompleteDrawCommand();
        }
        break;
    }

    case STATE_DRAW_DIM_ARCLEN_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hit) {
            if (dynamic_cast<CArcEntity*>(hit)) {
                m_pDimArcLenSrcEnt = hit;
                pDoc->DeselectAll();
                hit->m_bSelected = true;
                Invalidate(FALSE);
                pDoc->m_strCommandPrompt = L"Specify dimension arc position: ";
                SetDrawState(STATE_DRAW_DIM_ARCLEN_POS);
            } else {
                pDoc->m_strCommandPrompt = L"Not a arc. Select arc: ";
                CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
                if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
                    pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
                    int nLen = pDoc->m_strCommandPrompt.GetLength();
                    pFrame->m_wndCmdLine.SetSel(nLen, nLen);
                }
            }
        }
        break;
    }

    case STATE_DRAW_DIM_ARCLEN_POS: {
        if (m_pDimArcLenSrcEnt) {
            auto pArc = dynamic_cast<CArcEntity*>(m_pDimArcLenSrcEnt);
            if (!pArc) { CompleteDrawCommand(); break; }
            CPoint center = pArc->m_ptCenter;
            int arcRadius = pArc->m_nRadius;
            double dist = Distance(center, world);
            if (dist <= arcRadius + 1) {
                pDoc->m_strCommandPrompt = L"this position is illegal";
                Invalidate(FALSE);
                break;
            }
            double angStart = atan2((double)(pArc->m_ptStart.y - center.y), (double)(pArc->m_ptStart.x - center.x));
            double angEnd = atan2((double)(pArc->m_ptEnd.y - center.y), (double)(pArc->m_ptEnd.x - center.x));
            double sweep = angEnd - angStart;
            if (sweep < 0) sweep += 2*M_PI;
            double angMid = atan2((double)(pArc->m_ptMid.y - center.y), (double)(pArc->m_ptMid.x - center.x));
            double midSweep = angMid - angStart;
            if (midSweep < 0) midSweep += 2*M_PI;
            TRACE(L"[ARC DIM] angS=%.2f angE=%.2f angM=%.2f sweep=%.2f midSweep=%.2f swap=%d rad=%d\n",
                  angStart*180/M_PI, angEnd*180/M_PI, angMid*180/M_PI,
                  sweep*180/M_PI, midSweep*180/M_PI, (midSweep > sweep) ? 1 : 0, arcRadius);
            if (midSweep > sweep) {
                double tmp = angStart; angStart = angEnd; angEnd = tmp;
                sweep = 2*M_PI - sweep;
            }
            CDimArcLengthEntity* pDim = new CDimArcLengthEntity(center, arcRadius, (int)dist, angStart, angEnd, sweep);
            pDim->m_bTextPlaced = false;
            pDim->m_color = m_currentColor;
            pDim->m_nLineStyle = m_currentLineStyle;
            pDim->m_nLineWidth = m_currentLineWidth;
            pDoc->AddEntity(pDim);
            pDim->m_bSelected = true;
            m_pPendingDim = pDim;
            m_pDimArcLenSrcEnt = nullptr;
            pDoc->m_strCommandPrompt = L"Place dimension text: ";
            SetDrawState(STATE_DRAW_TEXT_POS);
        } else {
            CompleteDrawCommand();
        }
        break;
    }

    case STATE_DRAW_DIM_COORD_PICK: {
        m_ptCoordPoint = world;
        m_tempPts.push_back(world);
        m_bCoordDimMode = true;
        pDoc->m_strCommandPrompt = L"Place coordinate text: ";
        SetDrawState(STATE_DRAW_TEXT_POS);
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
    m_pPendingDim = nullptr;
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

    case VK_F3:
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
void CLargeHWView::OnDrawLine()       { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_LINE; SetDrawState(STATE_DRAW_LINE_P1); }
void CLargeHWView::OnDrawCircle()     { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_CIRCLE; SetDrawState(STATE_DRAW_CIRCLE_CENTER); }
void CLargeHWView::OnDrawArc()        { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_ARC; SetDrawState(STATE_DRAW_ARC_P1); }
void CLargeHWView::OnDrawRectangle()  { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_RECTANGLE; SetDrawState(STATE_DRAW_RECT_P1); }
void CLargeHWView::OnDrawEllipse()    { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_ELLIPSE; SetDrawState(STATE_DRAW_ELLIPSE_CENTER); }
void CLargeHWView::OnDrawPolyline()   { m_tempPts.clear(); m_bPolylineClose = false; m_nLastCommandID = ID_DRAW_POLYLINE; SetDrawState(STATE_DRAW_POLYLINE_POINT); }
void CLargeHWView::OnDrawText()       { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_TEXT; SetDrawState(STATE_DRAW_TEXT_POS); }
void CLargeHWView::OnDrawPolygon()    { m_tempPts.clear(); m_nPolygonSides = 6; m_nLastCommandID = ID_DRAW_POLYGON; SetDrawState(STATE_DRAW_POLYGON_CENTER); }
void CLargeHWView::OnDrawDimLength()  { OnDrawDimLengthAligned(); }
void CLargeHWView::OnDrawDimLengthAligned() { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_DIMENSION_LENGTH; m_nLastDimMode = 0; SetDrawState(STATE_DRAW_DIM_LENGTH_P1); }
void CLargeHWView::OnDrawDimLengthHoriz() { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_DIMENSION_LENGTH; m_nLastDimMode = 1; SetDrawState(STATE_DRAW_DIM_LENGTH_P1); }
void CLargeHWView::OnDrawDimLengthVert()  { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_DIMENSION_LENGTH; m_nLastDimMode = 2; SetDrawState(STATE_DRAW_DIM_LENGTH_P1); }
void CLargeHWView::OnDrawDimAngle()   
{
    m_tempPts.clear();
    m_nLastCommandID = ID_DRAW_DIMENSION_ANGLE;
    m_pDimEnt1 = nullptr; m_pDimEnt2 = nullptr;
    // create temporary split segments up-front so user can click/select split pieces immediately
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        // if there are old temp splits, remove them first
        if (!m_tempSplitNewIDs.empty()) {
            RestoreTemporarySplits(pDoc, m_tempSplitNewIDs);
            m_tempSplitNewIDs.clear();
        }
        // create temporary split segments but do NOT select them; selection should occur on user clicks
        CreateTemporarySplits(pDoc, m_tempSplitNewIDs);
    }
    SetDrawState(STATE_DRAW_DIM_ANGLE_SELECT_E1);
}

void CLargeHWView::OnDrawDimRadius()
{
    m_tempPts.clear();
    m_nLastCommandID = ID_DRAW_DIMENSION_RADIUS;
    m_pDimRadiusSrcEnt = nullptr;
    m_pPendingDim = nullptr;
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt = L"Select circle or arc: ";
    }
    SetDrawState(STATE_DRAW_DIM_RADIUS_SELECT);
}

void CLargeHWView::OnDrawDimDiameter()
{
    m_tempPts.clear();
    m_nLastCommandID = ID_DRAW_DIMENSION_DIAMETER;
    m_pDimDiamSrcEnt = nullptr;
    m_pPendingDim = nullptr;
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt = L"Select circle or arc: ";
    }
    SetDrawState(STATE_DRAW_DIM_DIAMETER_SELECT);
}

void CLargeHWView::OnDrawDimArcLength()
{
    m_tempPts.clear();
    m_nLastCommandID = ID_DRAW_DIMENSION_ARCLENGTH;
    m_pDimArcLenSrcEnt = nullptr;
    m_pPendingDim = nullptr;
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt = L"Select arc: ";
    }
    SetDrawState(STATE_DRAW_DIM_ARCLEN_SELECT);
}

void CLargeHWView::OnDrawDimCoordinate()
{
    m_tempPts.clear();
    m_nLastCommandID = ID_DRAW_DIMENSION_COORDINATE;
    m_bCoordDimMode = true;
    m_ptCoordPoint = CPoint(0, 0);
    m_pPendingDim = nullptr;
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        pDoc->m_strCommandPrompt = L"Specify point: ";
    }
    SetDrawState(STATE_DRAW_DIM_COORD_PICK);
}

// ============================================================
// Modify command handlers
// ============================================================
void CLargeHWView::OnModifyMove()
{
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
    CLargeHWDoc* pDoc = GetDocument();
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyDelete called, selected=%d\n", nSel);
    pDoc->DeleteSelected();
    Invalidate(FALSE);
}

void CLargeHWView::OnModifyRotate()
{
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
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->Undo();
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::OnEditRedo()
{
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
    m_tempPts.clear();
    m_nLastCommandID = ID_VIEW_ZOOM_WINDOW;
    SetDrawState(STATE_ZOOM_WINDOW_P1);
}

void CLargeHWView::OnViewPan()
{
    m_nLastCommandID = ID_VIEW_PAN;
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_strCommandPrompt = L"PAN: Hold and drag middle mouse button to pan. Press Esc to cancel.";
    UpdateStatusBar();
}

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
    m_bCoordDimMode = false;
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

    // Coordinate input (comma / @ / polar) - convert world → screen → click
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

    // --- Dimension commands ---
    } else if (cmd == L"DIMRAD" || cmd == L"DIMRADIUS" || cmd == L"DRA") {
        OnDrawDimRadius();
    } else if (cmd == L"DIMDIA" || cmd == L"DIMDIAMETER" || cmd == L"DDI") {
        OnDrawDimDiameter();
    } else if (cmd == L"DIMARC" || cmd == L"DIMARCLEN" || cmd == L"DAR") {
        OnDrawDimArcLength();
    } else if (cmd == L"DIMCOORD" || cmd == L"DIMCOORDINATE" || cmd == L"DOR") {
        OnDrawDimCoordinate();

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
