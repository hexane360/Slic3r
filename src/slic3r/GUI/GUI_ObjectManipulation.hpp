#ifndef slic3r_GUI_ObjectManipulation_hpp_
#define slic3r_GUI_ObjectManipulation_hpp_

#include <memory>

#include "GUI_ObjectSettings.hpp"
#include "GLCanvas3D.hpp"

class wxStaticText;

namespace Slic3r {
namespace GUI {


class ObjectManipulation : public OG_Settings
{
#if ENABLE_IMPROVED_SIDEBAR_OBJECTS_MANIPULATION
    struct Cache
    {
        Vec3d position;
        Vec3d rotation;
        Vec3d scale;
        Vec3d size;

        std::string move_label_string;
        std::string rotate_label_string;
        std::string scale_label_string;

        int object_idx;
        int instance_idx;

        Vec3d instance_box_size;

        Cache() : position(Vec3d(DBL_MAX, DBL_MAX, DBL_MAX)) , rotation(Vec3d(DBL_MAX, DBL_MAX, DBL_MAX))
            , scale(Vec3d(DBL_MAX, DBL_MAX, DBL_MAX)) , size(Vec3d(DBL_MAX, DBL_MAX, DBL_MAX))
            , move_label_string("") , rotate_label_string("") , scale_label_string("")
            , object_idx(-1)
            , instance_idx(-1)
        {
        }
    };

    Cache m_cache;
#else
    Vec3d       m_cache_position{ 0., 0., 0. };
    Vec3d       m_cache_rotation{ 0., 0., 0. };
    Vec3d       m_cache_scale{ 100., 100., 100. };
    Vec3d       m_cache_size{ 0., 0., 0. };
#endif // ENABLE_IMPROVED_SIDEBAR_OBJECTS_MANIPULATION

    wxStaticText*   m_move_Label = nullptr;
    wxStaticText*   m_scale_Label = nullptr;
    wxStaticText*   m_rotate_Label = nullptr;

#if !ENABLE_IMPROVED_SIDEBAR_OBJECTS_MANIPULATION
    // Needs to be updated from OnIdle?
    bool            m_dirty = false;
#endif // !ENABLE_IMPROVED_SIDEBAR_OBJECTS_MANIPULATION
    // Cached labels for the delayed update, not localized!
    std::string     m_new_move_label_string;
	std::string     m_new_rotate_label_string;
	std::string     m_new_scale_label_string;
    Vec3d           m_new_position;
    Vec3d           m_new_rotation;
    Vec3d           m_new_scale;
    Vec3d           m_new_size;
    bool            m_new_enabled;

public:
    ObjectManipulation(wxWindow* parent);
    ~ObjectManipulation() {}

    void        Show(const bool show) override;
    bool        IsShown() override;
    void        UpdateAndShow(const bool show) override;

    void        update_settings_value(const GLCanvas3D::Selection& selection);

	// Called from the App to update the UI if dirty.
	void		update_if_dirty();

private:
    void reset_settings_value();

    // update size values after scale unit changing or "gizmos"
    void update_size_value(const Vec3d& size);
    // update rotation value after "gizmos"
    void update_rotation_value(const Vec3d& rotation);

    // change values 
    void    change_position_value(const Vec3d& position);
    void    change_rotation_value(const Vec3d& rotation);
    void    change_scale_value(const Vec3d& scale);
    void    change_size_value(const Vec3d& size);
};

}}

#endif // slic3r_GUI_ObjectManipulation_hpp_
