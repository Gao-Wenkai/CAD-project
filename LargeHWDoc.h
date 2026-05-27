// LargeHWDoc.h: CAD Document - Entity Storage, Undo/Redo, Serialization

#pragma once

#include "Entity.h"
#include <vector>
#include <memory>
#include <deque>

// Action record for undo/redo
struct CAction
{
    enum ActionType { ACT_ADD, ACT_DELETE, ACT_MODIFY, ACT_CREATE_MULTI, ACT_DELETE_MULTI, ACT_MODIFY_MULTI };

    ActionType              type;
    CEntity*                pEntity;        // single entity clone for basic actions
    int                     nEntityID;
    CPoint                  ptOld, ptNew;
    std::vector<int>        entityIDs;      // multi-entity action IDs
    std::vector<CEntity*>   savedEntities;  // multi-entity action clones
};

class CLargeHWDoc : public CDocument
{
protected:
    CLargeHWDoc() noexcept;
    DECLARE_DYNCREATE(CLargeHWDoc)

public:
    // Entity management
    void      AddEntity(CEntity* pEntity, bool bRecordUndo = true);
    void      RemoveEntity(int nEntityID, bool bRecordUndo = true);
    void      RemoveAllEntities();
    CEntity*  FindEntityByID(int nID) const;
    CEntity*  HitTestEntity(CPoint pt, double scale, CPoint offset) const;
    std::vector<CEntity*> HitTestEntities(CPoint pt, double scale, CPoint offset) const;
    int       SelectByPoint(CPoint pt, double scale, CPoint offset);
    int       SelectByWindow(CRect rcWindow, double scale, CPoint offset);
    void      DeselectAll();
    void      DeleteSelected();

    const std::vector<CEntity*>& GetEntities() const { return m_entities; }

    // Selected entity ops
    int       GetSelectedCount() const;
    void      MoveSelected(double dx, double dy);
    void      RotateSelected(CPoint base, double angle);
    void      ScaleSelected(CPoint base, double factor);
    void      MirrorSelected(CPoint p1, CPoint p2);
    void      EraseSelected();
    std::vector<CEntity*> GetSelectedEntities() const;

    // Snap data collection
    void      CollectSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const;

    // Grip edit undo
    void      RecordGripUndo(CEntity* pEntity);
    void      RecordModifyUndo();
    void      RecordCreateUndo(const std::vector<int>& entityIDs);

    // Undo/Redo
    void      Undo();
    void      Redo();
    bool      CanUndo() const { return !m_undoStack.empty(); }
    bool      CanRedo() const { return !m_redoStack.empty(); }

    // Properties
    COLORREF  GetCurrentColor() const { return m_currentColor; }
    void      SetCurrentColor(COLORREF color) { m_currentColor = color; }
    int       GetCurrentLineStyle() const { return m_currentLineStyle; }
    void      SetCurrentLineStyle(int style) { m_currentLineStyle = style; }
    int       GetCurrentLineWidth() const { return m_currentLineWidth; }
    void      SetCurrentLineWidth(int width) { m_currentLineWidth = width; }
    CString   GetCurrentLayer() const { return m_strCurrentLayer; }
    void      SetCurrentLayer(const CString& layer) { m_strCurrentLayer = layer; }
    const std::vector<CString>& GetLayers() const { return m_layers; }
    void      AddLayer(const CString& layer);

    // Doc state
    bool      IsModified() const { return m_bModified; }
    void      SetModified(bool b) { m_bModified = b; SetModifiedFlag(b); }

    // Snap settings
    bool      m_bObjectSnap;
    bool      m_bSnapEndpoint;
    bool      m_bSnapMidpoint;
    bool      m_bSnapCenter;
    bool      m_bSnapQuadrant;
    bool      m_bSnapNearest;

    // View transform params (written by View)
    double    m_dScale;
    double    m_dModelUnitScale;
    CPoint    m_ptOffset;
    bool      m_bShowGrid;
    bool      m_bSnapToGrid;
    bool      m_bOrthoMode;
    int       m_nGridSpacing;

    // Current draw command (written by View)
    CadDrawState m_drawState;

    // Command prompt (shown in command line)
    CString   m_strCommandPrompt;

public:
    virtual BOOL OnNewDocument();
    virtual void Serialize(CArchive& ar);
    virtual ~CLargeHWDoc();

#ifdef _DEBUG
    virtual void AssertValid() const;
    virtual void Dump(CDumpContext& dc) const;
#endif

protected:
    std::vector<CEntity*> m_entities;
    std::deque<CAction>   m_undoStack;
    std::deque<CAction>   m_redoStack;
    static const int MAX_UNDO = 50;

    COLORREF  m_currentColor;
    int       m_currentLineStyle;
    int       m_currentLineWidth;
    CString   m_strCurrentLayer;
    bool      m_bModified;
    std::vector<CString> m_layers;

    void ClearUndoStack();
    void ClearRedoStack();
    void PushUndo(const CAction& action);

    DECLARE_MESSAGE_MAP()
};
