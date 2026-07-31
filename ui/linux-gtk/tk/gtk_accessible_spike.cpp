#include "gtk_accessible_spike.h"

#include <gtk/gtk.h>

namespace tk::gtk4
{

namespace
{

// ---- Leaf virtual accessible node: the hardcoded button. Not a GtkWidget
//      — a bare GObject, since it has no real widget of its own. This part
//      of the original (bare-GObject-as-direct-child) attempt was fine on
//      its own; the problem was only ever how it gets discovered from its
//      parent (see TkAccessRoot below). ----

#define TK_TYPE_ACCESS_SPIKE_NODE (tk_access_spike_node_get_type())
G_DECLARE_FINAL_TYPE(TkAccessSpikeNode, tk_access_spike_node, TK, ACCESS_SPIKE_NODE, GObject)

struct _TkAccessSpikeNode
{
    GObject parent_instance;
    GtkWidget* host_widget; // borrowed — the real widget we're a virtual child of
    GtkATContext* at_context;
    GtkAccessibleRole role;
};

enum
{
    PROP_ACCESSIBLE_ROLE = 1,
};

void node_accessible_init(GtkAccessibleInterface* iface);

G_DEFINE_TYPE_WITH_CODE(TkAccessSpikeNode, tk_access_spike_node, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_ACCESSIBLE, node_accessible_init))

void tk_access_spike_node_init(TkAccessSpikeNode* self)
{
    self->role = GTK_ACCESSIBLE_ROLE_BUTTON;
}

void tk_access_spike_node_dispose(GObject* object)
{
    TkAccessSpikeNode* self = TK_ACCESS_SPIKE_NODE(object);
    g_clear_object(&self->at_context);
    G_OBJECT_CLASS(tk_access_spike_node_parent_class)->dispose(object);
}

// See qt_accessible_spike.cpp's equivalent comment: GtkAccessible installs
// an "accessible-role" property on the interface but provides no storage
// for it — every implementing class must supply its own get_property/
// set_property, or GLib logs a CRITICAL and gtk_at_context_create() ends
// up with a bogus role.
void node_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec)
{
    TkAccessSpikeNode* self = TK_ACCESS_SPIKE_NODE(object);
    if (prop_id == PROP_ACCESSIBLE_ROLE)
        g_value_set_enum(value, self->role);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

void node_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec)
{
    TkAccessSpikeNode* self = TK_ACCESS_SPIKE_NODE(object);
    if (prop_id == PROP_ACCESSIBLE_ROLE)
        self->role = static_cast<GtkAccessibleRole>(g_value_get_enum(value));
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

void tk_access_spike_node_class_init(TkAccessSpikeNodeClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose      = tk_access_spike_node_dispose;
    object_class->get_property = node_get_property;
    object_class->set_property = node_set_property;
    g_object_class_override_property(object_class, PROP_ACCESSIBLE_ROLE, "accessible-role");
}

GtkATContext* node_get_at_context(GtkAccessible* accessible)
{
    TkAccessSpikeNode* self = TK_ACCESS_SPIKE_NODE(accessible);
    if (!self->at_context)
    {
        self->at_context = gtk_at_context_create(
            self->role, accessible, gtk_widget_get_display(self->host_widget));
    }
    return self->at_context ? static_cast<GtkATContext*>(g_object_ref(self->at_context))
                            : nullptr;
}

gboolean node_get_platform_state(GtkAccessible*, GtkAccessiblePlatformState state)
{
    return state == GTK_ACCESSIBLE_PLATFORM_STATE_FOCUSABLE ? TRUE : FALSE;
}

GtkAccessible* node_get_accessible_parent(GtkAccessible* accessible)
{
    TkAccessSpikeNode* self = TK_ACCESS_SPIKE_NODE(accessible);
    return GTK_ACCESSIBLE(g_object_ref(self->host_widget));
}

GtkAccessible* node_get_first_accessible_child(GtkAccessible*)
{
    return nullptr; // leaf
}

GtkAccessible* node_get_next_accessible_sibling(GtkAccessible*)
{
    return nullptr; // spike: only one virtual child for now
}

gboolean node_get_bounds(GtkAccessible* accessible, int* x, int* y, int* width, int* height)
{
    TkAccessSpikeNode* self = TK_ACCESS_SPIKE_NODE(accessible);
    *x      = 0;
    *y      = 0;
    *width  = gtk_widget_get_width(self->host_widget);
    *height = gtk_widget_get_height(self->host_widget);
    return TRUE;
}

char* node_get_accessible_id(GtkAccessible*)
{
    return g_strdup("tesseract-access-spike-button");
}

void node_accessible_init(GtkAccessibleInterface* iface)
{
    iface->get_at_context              = node_get_at_context;
    iface->get_platform_state          = node_get_platform_state;
    iface->get_accessible_parent       = node_get_accessible_parent;
    iface->get_first_accessible_child  = node_get_first_accessible_child;
    iface->get_next_accessible_sibling = node_get_next_accessible_sibling;
    iface->get_bounds                  = node_get_bounds;
    iface->get_accessible_id           = node_get_accessible_id;
}

// ---- Real (invisible, zero-size) marker widget: a genuine GtkWidget
//      child, added via gtk_overlay_add_overlay(), so GtkOverlay's own
//      default (real-widget-tree-walking) accessible behavior discovers it
//      automatically — no need to touch GtkOverlay itself. This widget
//      then reimplements GtkAccessible on top of GtkWidget's own
//      implementation (chaining up via g_type_interface_peek_parent() for
//      everything except get_first_accessible_child/
//      get_next_accessible_sibling, which fan out to the virtual button
//      instead of GtkWidget's default "walk my real children" behavior —
//      this marker has none). ----

#define TK_TYPE_ACCESS_ROOT (tk_access_root_get_type())
G_DECLARE_FINAL_TYPE(TkAccessRoot, tk_access_root, TK, ACCESS_ROOT, GtkWidget)

struct _TkAccessRoot
{
    GtkWidget parent_instance;
    TkAccessSpikeNode* button; // owned, created lazily
};

GtkAccessibleInterface* widget_accessible_parent_iface = nullptr;

void root_accessible_init(GtkAccessibleInterface* iface);

G_DEFINE_TYPE_WITH_CODE(TkAccessRoot, tk_access_root, GTK_TYPE_WIDGET,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_ACCESSIBLE, root_accessible_init))

void tk_access_root_init(TkAccessRoot*) {}

void tk_access_root_dispose(GObject* object)
{
    TkAccessRoot* self = TK_ACCESS_ROOT(object);
    g_clear_object(&self->button);
    G_OBJECT_CLASS(tk_access_root_parent_class)->dispose(object);
}

// Zero size unconditionally: this widget exists only to carry an
// accessible identity, never to be painted or affect layout.
void root_measure(GtkWidget*, GtkOrientation, int, int* minimum, int* natural,
                  int* minimum_baseline, int* natural_baseline)
{
    *minimum = *natural = 0;
    *minimum_baseline = *natural_baseline = -1;
}

void tk_access_root_class_init(TkAccessRootClass* klass)
{
    G_OBJECT_CLASS(klass)->dispose = tk_access_root_dispose;
    GtkWidgetClass* widget_class   = GTK_WIDGET_CLASS(klass);
    widget_class->measure          = root_measure;
    gtk_widget_class_set_accessible_role(widget_class, GTK_ACCESSIBLE_ROLE_GROUP);
}

GtkATContext* root_get_at_context(GtkAccessible* self)
{
    return widget_accessible_parent_iface->get_at_context(self);
}
gboolean root_get_platform_state(GtkAccessible* self, GtkAccessiblePlatformState state)
{
    return widget_accessible_parent_iface->get_platform_state(self, state);
}
GtkAccessible* root_get_accessible_parent(GtkAccessible* self)
{
    return widget_accessible_parent_iface->get_accessible_parent(self);
}
gboolean root_get_bounds(GtkAccessible* self, int* x, int* y, int* width, int* height)
{
    return widget_accessible_parent_iface->get_bounds(self, x, y, width, height);
}
char* root_get_accessible_id(GtkAccessible* self)
{
    return widget_accessible_parent_iface->get_accessible_id
             ? widget_accessible_parent_iface->get_accessible_id(self)
             : nullptr;
}

GtkAccessible* root_get_first_accessible_child(GtkAccessible* self)
{
    TkAccessRoot* root = TK_ACCESS_ROOT(self);
    if (!root->button)
    {
        root->button = TK_ACCESS_SPIKE_NODE(g_object_new(TK_TYPE_ACCESS_SPIKE_NODE, nullptr));
        root->button->host_widget = GTK_WIDGET(self);
        gtk_accessible_update_property(GTK_ACCESSIBLE(root->button),
                                       GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Tesseract accessibility spike button", -1);
    }
    return GTK_ACCESSIBLE(g_object_ref(root->button));
}
GtkAccessible* root_get_next_accessible_sibling(GtkAccessible*)
{
    return nullptr; // spike: only one virtual child per marker for now
}

void root_accessible_init(GtkAccessibleInterface* iface)
{
    widget_accessible_parent_iface =
        static_cast<GtkAccessibleInterface*>(g_type_interface_peek_parent(iface));

    iface->get_at_context              = root_get_at_context;
    iface->get_platform_state          = root_get_platform_state;
    iface->get_accessible_parent       = root_get_accessible_parent;
    iface->get_bounds                  = root_get_bounds;
    iface->get_accessible_id           = root_get_accessible_id;
    iface->get_first_accessible_child  = root_get_first_accessible_child;
    iface->get_next_accessible_sibling = root_get_next_accessible_sibling;
}

} // namespace

void attach_accessible_spike(GtkOverlay* overlay)
{
    GtkWidget* marker = GTK_WIDGET(g_object_new(TK_TYPE_ACCESS_ROOT, nullptr));
    gtk_overlay_add_overlay(overlay, marker);
    gtk_overlay_set_measure_overlay(overlay, marker, FALSE);
}

} // namespace tk::gtk4
