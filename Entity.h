// Entity.h: 2D CAD Entity Class Hierarchy
// AutoCAD-style entity model: base class + geometry type derivatives

#pragma once
#include <afx.h>
#include <vector>
#include <memory>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// -----------------------------------------------------------
// Enums
// -----------------------------------------------------------

// Entity geometry type
enum EntityType {
    ENT_LINE = 0,
    ENT_CIRCLE,
    ENT_ARC,
    ENT_RECTANGLE,
    ENT_POLYGON,
    ENT_ELLIPSE,
    ENT_POLYLINE,
    ENT_TEXT,
    ENT_POINT
};

// Drawing interaction state -- simulates AutoCAD command-line prompt flow
enum CadDrawState {
    STATE_IDLE = 0,
    STATE_DRAW_LINE_P1,
    STATE_DRAW_LINE_P2,
    STATE_DRAW_CIRCLE_CENTER,
    STATE_DRAW_CIRCLE_RADIUS,
    STATE_DRAW_ARC_P1,
    STATE_DRAW_ARC_P2,
    STATE_DRAW_ARC_P3,
    STATE_DRAW_ARC_PREVIEW,
    STATE_DRAW_RECT_P1,
    STATE_DRAW_RECT_P2,
    STATE_DRAW_POLYGON_CENTER,
    STATE_DRAW_POLYGON_RADIUS,
    STATE_DRAW_ELLIPSE_CENTER,
    STATE_DRAW_ELLIPSE_RADIUS,
    STATE_DRAW_POLYLINE_POINT,
    STATE_DRAW_TEXT_POS,
    STATE_MOVE_SELECT,
    STATE_MOVE_BASE,
    STATE_MOVE_DEST,
    STATE_COPY_SELECT,
    STATE_COPY_BASE,
    STATE_COPY_DEST,
    STATE_ROTATE_SELECT,
    STATE_ROTATE_CENTER,
    STATE_ROTATE_ANGLE,
    STATE_SCALE_SELECT,
    STATE_SCALE_BASE,
    STATE_SCALE_FACTOR,
    STATE_WINDOW_SELECT,
    STATE_MIRROR_SELECT,
    STATE_MIRROR_P1,
    STATE_MIRROR_P2,
    STATE_OFFSET_SELECT,
    STATE_OFFSET_DIST,
    STATE_CHAMFER_SELECT_FIRST,
    STATE_CHAMFER_SELECT_SECOND,
    STATE_FILLET_SELECT_FIRST,
    STATE_FILLET_SELECT_SECOND,
    STATE_ARRAY_SELECT,
    STATE_ARRAY_ROWS,
    STATE_ARRAY_COLUMNS,
    STATE_ARRAY_ROW_SPACING,
    STATE_ARRAY_COLUMN_SPACING,
    STATE_ZOOM_WINDOW_P1,
    STATE_ZOOM_WINDOW_P2,
};

// Object snap type
enum SnapType {
    SNAP_NONE = 0,
    SNAP_ENDPOINT,
    SNAP_MIDPOINT,
    SNAP_CENTER,
    SNAP_QUADRANT,
    SNAP_INTERSECTION,
    SNAP_NEAREST,
    SNAP_GRID
};

// -----------------------------------------------------------
// CEntity -- abstract base class for all entities
// -----------------------------------------------------------
class CEntity : public CObject
{
    DECLARE_SERIAL(CEntity)
public:
    CEntity();
    virtual ~CEntity();

    int         m_nID;
    EntityType  m_Type;
    COLORREF    m_color;
    int         m_nLineStyle;       // PS_SOLID, PS_DASH, PS_DOT, PS_DASHDOT
    int         m_nLineWidth;
    CString     m_strLayer;
    bool        m_bSelected;
    bool        m_bVisible;
    bool        m_bUseLayerColor;

    virtual void   Draw(CDC* pDC, double scale, CPoint offset);
    virtual bool   HitTest(CPoint pt, double scale, CPoint offset);
    virtual void   Move(double dx, double dy);
    virtual void   Rotate(CPoint base, double angle);
    virtual void   Scale(CPoint base, double factor);
    virtual void   Mirror(CPoint p1, CPoint p2);
    virtual CRect  GetBounds();
    virtual CEntity* Clone() const;
    virtual void   Serialize(CArchive& ar);

    virtual int    GetGripCount();
    virtual CPoint GetGrip(int index);
    virtual void   SetGrip(int index, CPoint pt);
    virtual int    HitTestGrip(CPoint pt, double scale, CPoint offset);
    virtual void   GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const;

    static int     m_nNextID;
    CPoint ToWorld(CPoint screen, double scale, CPoint offset) const;
    CPoint ToScreen(CPoint world, double scale, CPoint offset) const;
    CRect  ToScreenRect(CRect world, double scale, CPoint offset) const;

    void ToggleSelect() { m_bSelected = !m_bSelected; }
};

// -----------------------------------------------------------
// CLineEntity -- Line
// -----------------------------------------------------------
class CLineEntity : public CEntity
{
    DECLARE_SERIAL(CLineEntity)
public:
    CLineEntity();
    CLineEntity(CPoint start, CPoint end);

    CPoint m_ptStart, m_ptEnd;

    virtual void   Draw(CDC* pDC, double scale, CPoint offset) override;
    virtual bool   HitTest(CPoint pt, double scale, CPoint offset) override;
    virtual void   Move(double dx, double dy) override;
    virtual void   Rotate(CPoint base, double angle) override;
    virtual void   Scale(CPoint base, double factor) override;
    virtual void   Mirror(CPoint p1, CPoint p2) override;
    virtual CRect  GetBounds() override;
    virtual CEntity* Clone() const override;
    virtual void   Serialize(CArchive& ar) override;

    virtual int    GetGripCount() override { return 2; }
    virtual CPoint GetGrip(int index) override;
    virtual void   SetGrip(int index, CPoint pt) override;
    virtual void   GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const override;
};

// -----------------------------------------------------------
// CCircleEntity -- Circle
// -----------------------------------------------------------
class CCircleEntity : public CEntity
{
    DECLARE_SERIAL(CCircleEntity)
public:
    CCircleEntity();
    CCircleEntity(CPoint center, int radius);

    CPoint m_ptCenter;
    int    m_nRadius;

    virtual void   Draw(CDC* pDC, double scale, CPoint offset) override;
    virtual bool   HitTest(CPoint pt, double scale, CPoint offset) override;
    virtual void   Move(double dx, double dy) override;
    virtual void   Rotate(CPoint base, double angle) override;
    virtual void   Scale(CPoint base, double factor) override;
    virtual void   Mirror(CPoint p1, CPoint p2) override;
    virtual CRect  GetBounds() override;
    virtual CEntity* Clone() const override;
    virtual void   Serialize(CArchive& ar) override;

    virtual int    GetGripCount() override { return 5; }
    virtual CPoint GetGrip(int index) override;
    virtual void   SetGrip(int index, CPoint pt) override;
    virtual void   GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const override;
};

// -----------------------------------------------------------
// CArcEntity -- Arc (3-point)
// -----------------------------------------------------------
class CArcEntity : public CEntity
{
    DECLARE_SERIAL(CArcEntity)
public:
    CArcEntity();
    CArcEntity(CPoint start, CPoint mid, CPoint end);

    CPoint m_ptStart, m_ptMid, m_ptEnd;
    CPoint m_ptCenter;
    int    m_nRadius;

    void CalcGeometry();
    void SetArcByCenter(CPoint center, int radius, double angStart, double angEnd);

    virtual void   Draw(CDC* pDC, double scale, CPoint offset) override;
    virtual bool   HitTest(CPoint pt, double scale, CPoint offset) override;
    virtual void   Move(double dx, double dy) override;
    virtual void   Rotate(CPoint base, double angle) override;
    virtual void   Scale(CPoint base, double factor) override;
    virtual void   Mirror(CPoint p1, CPoint p2) override;
    virtual CRect  GetBounds() override;
    virtual CEntity* Clone() const override;
    virtual void   Serialize(CArchive& ar) override;

    virtual int    GetGripCount() override { return 3; }
    virtual CPoint GetGrip(int index) override;
    virtual void   SetGrip(int index, CPoint pt) override;
    virtual void   GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const override;
};

// -----------------------------------------------------------
// CRectangleEntity -- Rectangle
// -----------------------------------------------------------
class CRectangleEntity : public CEntity
{
    DECLARE_SERIAL(CRectangleEntity)
public:
    CRectangleEntity();
    CRectangleEntity(CPoint corner1, CPoint corner2);

    CPoint m_ptCorner1, m_ptCorner2;

    virtual void   Draw(CDC* pDC, double scale, CPoint offset) override;
    virtual bool   HitTest(CPoint pt, double scale, CPoint offset) override;
    virtual void   Move(double dx, double dy) override;
    virtual void   Rotate(CPoint base, double angle) override;
    virtual void   Scale(CPoint base, double factor) override;
    virtual void   Mirror(CPoint p1, CPoint p2) override;
    virtual CRect  GetBounds() override;
    virtual CEntity* Clone() const override;
    virtual void   Serialize(CArchive& ar) override;

    virtual int    GetGripCount() override { return 4; }
    virtual CPoint GetGrip(int index) override;
    virtual void   SetGrip(int index, CPoint pt) override;
    virtual void   GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const override;
};

// -----------------------------------------------------------
// CPolygonEntity -- Regular Polygon (3-12 sides)
// -----------------------------------------------------------
class CPolygonEntity : public CEntity
{
    DECLARE_SERIAL(CPolygonEntity)
public:
    CPolygonEntity();
    CPolygonEntity(CPoint center, int radius, int sides);

    CPoint m_ptCenter;
    int    m_nRadius;
    int    m_nSides;

    void GetVertices(std::vector<CPoint>& vertices) const;

    virtual void   Draw(CDC* pDC, double scale, CPoint offset) override;
    virtual bool   HitTest(CPoint pt, double scale, CPoint offset) override;
    virtual void   Move(double dx, double dy) override;
    virtual void   Rotate(CPoint base, double angle) override;
    virtual void   Scale(CPoint base, double factor) override;
    virtual void   Mirror(CPoint p1, CPoint p2) override;
    virtual CRect  GetBounds() override;
    virtual CEntity* Clone() const override;
    virtual void   Serialize(CArchive& ar) override;

    virtual int    GetGripCount() override { return m_nSides + 1; }
    virtual CPoint GetGrip(int index) override;
    virtual void   SetGrip(int index, CPoint pt) override;
    virtual void   GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const override;
};

// -----------------------------------------------------------
// CEllipseEntity -- Ellipse
// -----------------------------------------------------------
class CEllipseEntity : public CEntity
{
    DECLARE_SERIAL(CEllipseEntity)
public:
    CEllipseEntity();
    CEllipseEntity(CPoint center, int rx, int ry);

    CPoint m_ptCenter;
    int    m_nRadiusX;
    int    m_nRadiusY;

    virtual void   Draw(CDC* pDC, double scale, CPoint offset) override;
    virtual bool   HitTest(CPoint pt, double scale, CPoint offset) override;
    virtual void   Move(double dx, double dy) override;
    virtual void   Rotate(CPoint base, double angle) override;
    virtual void   Scale(CPoint base, double factor) override;
    virtual void   Mirror(CPoint p1, CPoint p2) override;
    virtual CRect  GetBounds() override;
    virtual CEntity* Clone() const override;
    virtual void   Serialize(CArchive& ar) override;

    virtual int    GetGripCount() override { return 5; }
    virtual CPoint GetGrip(int index) override;
    virtual void   SetGrip(int index, CPoint pt) override;
    virtual void   GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const override;
};

// -----------------------------------------------------------
// CPolylineEntity -- Polyline (multi-segment)
// -----------------------------------------------------------
class CPolylineEntity : public CEntity
{
    DECLARE_SERIAL(CPolylineEntity)
public:
    CPolylineEntity();
    CPolylineEntity(const std::vector<CPoint>& points);

    std::vector<CPoint> m_vertices;
    bool m_bClosed;

    virtual void   Draw(CDC* pDC, double scale, CPoint offset) override;
    virtual bool   HitTest(CPoint pt, double scale, CPoint offset) override;
    virtual void   Move(double dx, double dy) override;
    virtual void   Rotate(CPoint base, double angle) override;
    virtual void   Scale(CPoint base, double factor) override;
    virtual void   Mirror(CPoint p1, CPoint p2) override;
    virtual CRect  GetBounds() override;
    virtual CEntity* Clone() const override;
    virtual void   Serialize(CArchive& ar) override;

    virtual int    GetGripCount() override { return (int)m_vertices.size(); }
    virtual CPoint GetGrip(int index) override;
    virtual void   SetGrip(int index, CPoint pt) override;
    virtual void   GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const override;

    void AddVertex(CPoint pt) { m_vertices.push_back(pt); }
    CPoint GetLastVertex() const { return m_vertices.empty() ? CPoint(0,0) : m_vertices.back(); }
    int    GetVertexCount() const { return (int)m_vertices.size(); }
};

// -----------------------------------------------------------
// CPointEntity -- Point marker
// -----------------------------------------------------------
class CPointEntity : public CEntity
{
    DECLARE_SERIAL(CPointEntity)
public:
    CPointEntity();
    CPointEntity(CPoint pos);

    CPoint m_ptPosition;

    virtual void   Draw(CDC* pDC, double scale, CPoint offset) override;
    virtual bool   HitTest(CPoint pt, double scale, CPoint offset) override;
    virtual void   Move(double dx, double dy) override;
    virtual void   Rotate(CPoint base, double angle) override;
    virtual void   Scale(CPoint base, double factor) override;
    virtual void   Mirror(CPoint p1, CPoint p2) override;
    virtual CRect  GetBounds() override;
    virtual CEntity* Clone() const override;
    virtual void   Serialize(CArchive& ar) override;

    virtual int    GetGripCount() override { return 1; }
    virtual CPoint GetGrip(int index) override;
    virtual void   SetGrip(int index, CPoint pt) override;
    virtual void   GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const override;
};

// -----------------------------------------------------------
// CTextEntity -- Single-line text
// -----------------------------------------------------------
class CTextEntity : public CEntity
{
    DECLARE_SERIAL(CTextEntity)
public:
    CTextEntity();
    CTextEntity(CPoint pos, const CString& text, int height = 20);

    CPoint  m_ptPosition;
    CString m_strText;
    int     m_nHeight;
    double  m_dRotation;

    virtual void   Draw(CDC* pDC, double scale, CPoint offset) override;
    virtual bool   HitTest(CPoint pt, double scale, CPoint offset) override;
    virtual void   Move(double dx, double dy) override;
    virtual void   Rotate(CPoint base, double angle) override;
    virtual void   Scale(CPoint base, double factor) override;
    virtual void   Mirror(CPoint p1, CPoint p2) override;
    virtual CRect  GetBounds() override;
    virtual CEntity* Clone() const override;
    virtual void   Serialize(CArchive& ar) override;

    virtual int    GetGripCount() override { return 2; }
    virtual CPoint GetGrip(int index) override;
    virtual void   SetGrip(int index, CPoint pt) override;
    virtual void   GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const override;
};

// -----------------------------------------------------------
// Utility functions
// -----------------------------------------------------------
double PointToLineDistance(CPoint pt, CPoint lineStart, CPoint lineEnd);
bool PointOnRectEdge(CPoint pt, CRect rc, int tolerance = 5);

inline double Distance(CPoint p1, CPoint p2) {
    return sqrt(pow((double)(p1.x - p2.x), 2) + pow((double)(p1.y - p2.y), 2));
}

inline CPoint MidPoint(CPoint p1, CPoint p2) {
    return CPoint((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);
}

inline double DotProduct(CPoint p1, CPoint p2) {
    return (double)p1.x * p2.x + (double)p1.y * p2.y;
}

inline CPoint RotatePoint(CPoint pt, CPoint base, double angle) {
    double s = sin(angle), c = cos(angle);
    double dx = pt.x - base.x, dy = pt.y - base.y;
    return CPoint((int)(base.x + dx * c - dy * s + 0.5),
                  (int)(base.y + dx * s + dy * c + 0.5));
}

inline CPoint ScalePoint(CPoint pt, CPoint base, double factor) {
    return CPoint((int)(base.x + (pt.x - base.x) * factor + 0.5),
                  (int)(base.y + (pt.y - base.y) * factor + 0.5));
}

inline CPoint MirrorPoint(CPoint pt, CPoint p1, CPoint p2) {
    double dx = (double)(p2.x - p1.x);
    double dy = (double)(p2.y - p1.y);
    double lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-9) return pt;
    double t = ((double)(pt.x - p1.x) * dx + (double)(pt.y - p1.y) * dy) / lenSq;
    double projX = p1.x + t * dx;
    double projY = p1.y + t * dy;
    return CPoint((int)(2.0 * projX - pt.x + 0.5), (int)(2.0 * projY - pt.y + 0.5));
}

inline double AngleBetween(CPoint p1, CPoint p2, CPoint p3) {
    double a = Distance(p2, p3), b = Distance(p1, p3), c = Distance(p1, p2);
    double val = (b * b + c * c - a * a) / (2.0 * b * c);
    if (val > 1.0) val = 1.0;
    if (val < -1.0) val = -1.0;
    return acos(val);
}
