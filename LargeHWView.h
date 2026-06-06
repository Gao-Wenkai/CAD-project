// LargeHWView.h: CAD View - Rendering Engine & Interaction State Machine

#pragma once

#include "Entity.h"
#include "LargeHWDoc.h"

class CLargeHWView : public CView
{
protected:
    CLargeHWView() noexcept;
    DECLARE_DYNCREATE(CLargeHWView)

public:
    struct ChamferSegmentRef
    {
        CEntity* pEntity;
        int      segmentIndex;
        CPoint   start;
        CPoint   end;

        ChamferSegmentRef() : pEntity(nullptr), segmentIndex(-1), start(0, 0), end(0, 0) {}
        bool IsValid() const { return pEntity != nullptr && segmentIndex >= 0; }
    };

    CLargeHWDoc* GetDocument() const;

    // Drawing state (temporary interaction data)
    std::vector<CPoint> m_tempPts;
    CPoint              m_ptCurrent;
    bool                m_bDrawing;
    bool                m_bDragging;
    CPoint              m_ptDragStart;
    int                 m_nGripIndex;
    CEntity*            m_pGripEntity;
    int                 m_nPolygonSides;
    bool                m_bArcAltHalf;      // Arc: toggle alternate half
    bool                m_bPolylineClose;   // PL: C key toggles close back to start
    // For angular dimension selection: hold selected line entities
    CEntity*            m_pDimEnt1;
    CEntity*            m_pDimEnt2;
    // Pending created dimension entity that awaits text placement
    CEntity*            m_pPendingDim;
    // If selecting a segment from a polyline, store original and index
    CEntity*            m_pDimEnt1Orig;
    CEntity*            m_pDimEnt2Orig;
    int                 m_nDimSegIndex1;
    int                 m_nDimSegIndex2;
    bool                m_bDimEnt1Temp;
    bool                m_bDimEnt2Temp;
    CEntity*            m_pDimRadiusSrcEnt; // The circle/arc selected for radius dim
    CEntity*            m_pDimDiamSrcEnt;   // The circle/arc selected for diameter dim
    CEntity*            m_pDimArcLenSrcEnt;  // The arc selected for arc length dim
    bool                m_bCoordDimMode;       // true during coordinate dimension command
    CPoint              m_ptCoordPoint;         // the point picked for coordinate dim
    // when entering STATE_DRAW_DIM_ANGLE_POS, ignore the immediate leftover click once
    bool                m_bJustEnteredAnglePos;

    // Temporary split state for angle-dimension command
    std::vector<int>    m_tempSplitNewIDs;                // IDs of temporary created split segments
    // Polyline / chamfer / fillet / array
    bool                m_bPolylineArcMode;
    int                 m_nPolylineWidth;
    int                 m_nPolylineStartWidth;
    int                 m_nPolylineEndWidth;
    CPolylineEntity*    m_pActivePolyline;
    CLineEntity*        m_pChamferFirst;
    ChamferSegmentRef   m_chamferFirstSegment;
    double              m_dChamferDistance;
    ChamferSegmentRef   m_filletFirstSegment;
    double              m_dFilletRadius;
    int                 m_nArrayRows;
    int                 m_nArrayColumns;
    double              m_dArrayRowSpacing;
    double              m_dArrayColumnSpacing;

    // Pan state
    bool                m_bPanning;
    CPoint              m_ptPanStart;
    CPoint              m_ptPanOffsetStart;

    // Snap state
    CPoint              m_ptSnapped;
    bool                m_bSnapActive;
    SnapType            m_nSnapType;

    // Last command tracking
    UINT                m_nLastCommandID;
    int                 m_nLastDimMode; // 0=aligned,1=horizontal,2=vertical

    COLORREF            m_currentColor;
    int                 m_currentLineStyle;
    int                 m_currentLineWidth;

    virtual void OnDraw(CDC* pDC);
    virtual BOOL PreCreateWindow(CREATESTRUCT& cs);

    // Mouse handlers
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnRButtonDown(UINT nFlags, CPoint point);
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    afx_msg void OnMButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnMButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnContextMenu(CWnd* pWnd, CPoint pt);

    // Draw commands
    afx_msg void OnDrawLine();
    afx_msg void OnDrawPolyline();
    afx_msg void OnDrawCircle();
    afx_msg void OnDrawArc();
    afx_msg void OnDrawRectangle();
    afx_msg void OnDrawPolygon();
    afx_msg void OnDrawEllipse();
    afx_msg void OnDrawText();
    afx_msg void OnDrawDimLength();
    afx_msg void OnDrawDimLengthAligned();
    afx_msg void OnDrawDimLengthHoriz();
    afx_msg void OnDrawDimLengthVert();
    afx_msg void OnDrawDimAngle();
    afx_msg void OnDrawDimRadius();
    afx_msg void OnDrawDimDiameter();
    afx_msg void OnDrawDimArcLength();
    afx_msg void OnDrawDimCoordinate();

    // Modify commands
    afx_msg void OnModifyMove();
    afx_msg void OnModifyCopy();
    afx_msg void OnModifyRotate();
    afx_msg void OnModifyScale();
    afx_msg void OnModifyDelete();
    afx_msg void OnModifyMirror();
    afx_msg void OnModifyOffset();
    afx_msg void OnModifyChamfer();
    afx_msg void OnModifyFillet();
    afx_msg void OnModifyArray();

    // Edit commands
    afx_msg void OnEditUndo();
    afx_msg void OnEditRedo();
    afx_msg void OnEditSelectAll();
    afx_msg void OnUpdateEditUndo(CCmdUI* pCmdUI);
    afx_msg void OnUpdateEditRedo(CCmdUI* pCmdUI);

    // Context menu
    afx_msg void OnContextRepeat();

    // View commands
    afx_msg void OnViewZoomExtents();
    afx_msg void OnViewZoomWindow();
    afx_msg void OnViewPan();
    afx_msg void OnViewGrid();
    afx_msg void OnViewSnap();
    afx_msg void OnViewOrtho();

    // Property commands
    afx_msg void OnColorRed();
    afx_msg void OnColorYellow();
    afx_msg void OnColorGreen();
    afx_msg void OnColorCyan();
    afx_msg void OnColorBlue();
    afx_msg void OnColorMagenta();
    afx_msg void OnColorWhite();
    afx_msg void OnLinetypeSolid();
    afx_msg void OnLinetypeDash();
    afx_msg void OnLinetypeDot();
    afx_msg void OnLinetypeDashDot();
    afx_msg void OnLineweight1();
    afx_msg void OnLineweight2();
    afx_msg void OnLineweight3();
    afx_msg void OnLineweight4();
    afx_msg void OnCancelCommand();
    afx_msg void OnFormatLayer();

    // SCR script commands
    afx_msg void OnScriptRun();
    afx_msg void OnScriptRecordStart();
    afx_msg void OnScriptRecordStop();
    afx_msg void OnUpdateScriptRun(CCmdUI* pCmdUI);
    afx_msg void OnUpdateScriptRecordStart(CCmdUI* pCmdUI);
    afx_msg void OnUpdateScriptRecordStop(CCmdUI* pCmdUI);

protected:
    virtual BOOL OnPreparePrinting(CPrintInfo* pInfo);
    virtual void OnBeginPrinting(CDC* pDC, CPrintInfo* pInfo);
    virtual void OnEndPrinting(CDC* pDC, CPrintInfo* pInfo);

public:
    virtual ~CLargeHWView();
    void SubmitCommandLineInput(const CString& strInput, bool bRecord = true);
    void ExecuteCommand(const CString& strCmd);
    void ProcessCoordinateInput(const CString& strInput);
    CPoint ParseCoordinate(const CString& str, CPoint ref) const;

#ifdef _DEBUG
    virtual void AssertValid() const;
    virtual void Dump(CDumpContext& dc) const;
#endif

protected:
    void DrawGrid(CDC* pDC);
    void DrawCrosshair(CDC* pDC, CPoint pt);
    void DrawEntities(CDC* pDC);
    void DrawPreview(CDC* pDC);
    void DrawUCSIcon(CDC* pDC);
    void DrawSnapMarker(CDC* pDC);

    CPoint  SnapToGrid(CPoint pt) const;
    CPoint  SnapToObjects(CPoint pt);
    CPoint  ApplyOrtho(CPoint pt, CPoint ref) const;
    CPoint  WorldToScreen(CPoint world) const;
    CPoint  ScreenToWorld(CPoint screen) const;
    void    UpdateStatusBar();
    void    SetDrawState(CadDrawState state);
    void    CompleteDrawCommand();
    void    RepeatLastCommand();
    CLineEntity* HitTestLineEntity(CPoint point) const;
    ChamferSegmentRef HitTestChamferSegment(CPoint point) const;
    bool    ApplyChamfer(CLineEntity* pFirst, CLineEntity* pSecond, double distance);
    bool    ApplyChamfer(const ChamferSegmentRef& first, const ChamferSegmentRef& second, double distance);
    bool    ApplyFillet(const ChamferSegmentRef& first, const ChamferSegmentRef& second, double radius);
    bool    UpdateArrayDefaultSpacingFromSelection();
    void    CreateRectangularArray(int rows, int columns, double rowSpacing, double columnSpacing);
    bool    ProcessArrayParameterInput(const CString& strInput);
    bool    CloseLineCommand();
    bool    FinishPolylineCommand(bool close);
    void    AddPolylinePoint(CPoint world);
    void    RecordScriptInput(const CString& strInput);
    bool    IsCoordinateInput(const CString& strInput) const;
    bool    ExecuteScriptFile(const CString& strPath);
    void    ExecuteScriptLine(const CString& strLine);
    bool    ExecuteDirectScriptCommand(const CString& strLine);
    void    TokenizeScriptLine(const CString& strLine, std::vector<CString>& tokens) const;
    CString StripScriptComment(const CString& strLine) const;
    CString FormatScriptPoint(CPoint pt) const;
    CString EscapeScriptText(const CString& strText) const;
    void    SyncCommandLinePrompt();
    bool    ShouldRecordPointForState(CadDrawState state) const;

    bool    m_bScriptRecording;
    bool    m_bRunningScript;
    bool    m_bSubmittingCommandLine;
    double  m_dScriptCoordinateScale;
    CString m_strScriptRecordPath;
    CFile   m_scriptRecordFile;

    DECLARE_MESSAGE_MAP()
};

#ifndef _DEBUG
inline CLargeHWDoc* CLargeHWView::GetDocument() const
   { return reinterpret_cast<CLargeHWDoc*>(m_pDocument); }
#endif
