/*
    Copyright (C) 2002-2003 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <cmath>
#include <climits>
#include <vector>
#include <fstream>

#include "pbd/stl_delete.h"
#include "pbd/memento_command.h"
#include "pbd/stacktrace.h"

#include "ardour/automation_list.h"
#include "ardour/dB.h"
#include "evoral/Curve.hpp"

#include "simplerect.h"
#include "automation_line.h"
#include "control_point.h"
#include "gui_thread.h"
#include "rgb_macros.h"
#include "ardour_ui.h"
#include "public_editor.h"
#include "utils.h"
#include "selection.h"
#include "time_axis_view.h"
#include "point_selection.h"
#include "automation_time_axis.h"

#include "ardour/event_type_map.h"
#include "ardour/session.h"

#include "i18n.h"

using namespace std;
using namespace ARDOUR;
using namespace PBD;
using namespace Editing;
using namespace Gnome; // for Canvas

/** @param converter A TimeConverter whose origin_b is the start time of the AutomationList in session frames.
 *  This will not be deleted by AutomationLine.
 */
AutomationLine::AutomationLine (const string& name, TimeAxisView& tv, ArdourCanvas::Group& parent,
		boost::shared_ptr<AutomationList> al,
		Evoral::TimeConverter<double, framepos_t>* converter)
	: trackview (tv)
	, _name (name)
	, alist (al)
	, _time_converter (converter ? converter : new Evoral::IdentityConverter<double, framepos_t>)
	, _parent_group (parent)
	, _offset (0)
	, _maximum_time (max_framepos)
{
	if (converter) {
		_time_converter = converter;
		_our_time_converter = false;
	} else {
		_time_converter = new Evoral::IdentityConverter<double, framepos_t>;
		_our_time_converter = true;
	}
	
	_visible = Line;

	update_pending = false;
	_uses_gain_mapping = false;
	no_draw = false;
	_is_boolean = false;
	terminal_points_can_slide = true;
	_height = 0;

	group = new ArdourCanvas::Group (parent);
	group->property_x() = 0.0;
	group->property_y() = 0.0;

	line = new ArdourCanvas::Line (*group);
	line->property_width_pixels() = (guint)1;
	line->set_data ("line", this);

	line->signal_event().connect (sigc::mem_fun (*this, &AutomationLine::event_handler));

	trackview.session()->register_with_memento_command_factory(alist->id(), this);

	if (alist->parameter().type() == GainAutomation ||
	    alist->parameter().type() == EnvelopeAutomation) {
		set_uses_gain_mapping (true);
	}

	interpolation_changed (alist->interpolation ());

	connect_to_list ();
}

AutomationLine::~AutomationLine ()
{
	vector_delete (&control_points);
	delete group;

	if (_our_time_converter) {
		delete _time_converter;
	}
}

bool
AutomationLine::event_handler (GdkEvent* event)
{
	return PublicEditor::instance().canvas_line_event (event, line, this);
}

void
AutomationLine::queue_reset ()
{
	if (!update_pending) {
		update_pending = true;
		Gtkmm2ext::UI::instance()->call_slot (invalidator (*this), boost::bind (&AutomationLine::reset, this));
	}
}

void
AutomationLine::show ()
{
	if (_visible & Line) {
		if (alist->interpolation() != AutomationList::Discrete) {
			line->show();
		} else {
			line->hide ();
		}
	} else {
		line->hide();
	}

	if (_visible & ControlPoints) {
		for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {
			(*i)->set_visible (true);
			(*i)->show ();
		}
	} else if (_visible & SelectedControlPoints) {
		for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {
			(*i)->set_visible ((*i)->get_selected());
		}
	} else {
		for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {
			(*i)->set_visible (false);
		}
	}
}

void
AutomationLine::hide ()
{
	set_visibility (VisibleAspects (0));
}

double
AutomationLine::control_point_box_size ()
{
	if (alist->interpolation() == AutomationList::Discrete) {
		return max((_height*4.0) / (double)(alist->parameter().max() - alist->parameter().min()),
				4.0);
	}

	if (_height > TimeAxisView::preset_height (HeightLarger)) {
		return 8.0;
	} else if (_height > (guint32) TimeAxisView::preset_height (HeightNormal)) {
		return 6.0;
	}
	return 4.0;
}

void
AutomationLine::set_height (guint32 h)
{
	if (h != _height) {
		_height = h;

		double bsz = control_point_box_size();

		for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {
			(*i)->set_size (bsz);
		}

		reset ();
	}
}

void
AutomationLine::set_line_color (uint32_t color)
{
	_line_color = color;
	line->property_fill_color_rgba() = color;
}

void
AutomationLine::set_uses_gain_mapping (bool yn)
{
	if (yn != _uses_gain_mapping) {
		_uses_gain_mapping = yn;
		reset ();
	}
}

ControlPoint*
AutomationLine::nth (uint32_t n)
{
	if (n < control_points.size()) {
		return control_points[n];
	} else {
		return 0;
	}
}

ControlPoint const *
AutomationLine::nth (uint32_t n) const
{
	if (n < control_points.size()) {
		return control_points[n];
	} else {
		return 0;
	}
}

void
AutomationLine::modify_point_y (ControlPoint& cp, double y)
{
	/* clamp y-coord appropriately. y is supposed to be a normalized fraction (0.0-1.0),
	   and needs to be converted to a canvas unit distance.
	*/

	y = max (0.0, y);
	y = min (1.0, y);
	y = _height - (y * _height);

	double const x = trackview.editor().frame_to_unit_unrounded (_time_converter->to((*cp.model())->when) - _offset);

	trackview.editor().session()->begin_reversible_command (_("automation event move"));
	trackview.editor().session()->add_command (
		new MementoCommand<AutomationList> (memento_command_binder(), &get_state(), 0)
		);

	cp.move_to (x, y, ControlPoint::Full);

	reset_line_coords (cp);

	if (line_points.size() > 1) {
		line->property_points() = line_points;
	}

	alist->freeze ();
	sync_model_with_view_point (cp, 0);
	alist->thaw ();

	update_pending = false;

	trackview.editor().session()->add_command (
		new MementoCommand<AutomationList> (memento_command_binder(), 0, &alist->get_state())
		);

	trackview.editor().session()->commit_reversible_command ();
	trackview.editor().session()->set_dirty ();
}

void
AutomationLine::reset_line_coords (ControlPoint& cp)
{
	if (cp.view_index() < line_points.size()) {
		line_points[cp.view_index()].set_x (cp.get_x());
		line_points[cp.view_index()].set_y (cp.get_y());
	}
}

void
AutomationLine::sync_model_with_view_points (list<ControlPoint*> cp, int64_t distance)
{
	update_pending = true;

	for (list<ControlPoint*>::iterator i = cp.begin(); i != cp.end(); ++i) {
		sync_model_with_view_point (**i, distance);
	}
}

string
AutomationLine::get_verbose_cursor_string (double fraction) const
{
	std::string s = fraction_to_string (fraction);
	if (_uses_gain_mapping) {
		s += " dB";
	}

	return s;
}

/**
 *  @param fraction y fraction
 *  @return string representation of this value, using dB if appropriate.
 */
string
AutomationLine::fraction_to_string (double fraction) const
{
	char buf[32];

	if (_uses_gain_mapping) {
		if (fraction == 0.0) {
			snprintf (buf, sizeof (buf), "-inf");
		} else {
			snprintf (buf, sizeof (buf), "%.1f", accurate_coefficient_to_dB (slider_position_to_gain_with_max (fraction, Config->get_max_gain())));
		}
	} else {
		view_to_model_coord_y (fraction);
		if (EventTypeMap::instance().is_integer (alist->parameter())) {
			snprintf (buf, sizeof (buf), "%d", (int)fraction);
		} else {
			snprintf (buf, sizeof (buf), "%.2f", fraction);
		}
	}

	return buf;
}


/**
 *  @param s Value string in the form as returned by fraction_to_string.
 *  @return Corresponding y fraction.
 */
double
AutomationLine::string_to_fraction (string const & s) const
{
	if (s == "-inf") {
		return 0;
	}

	double v;
	sscanf (s.c_str(), "%lf", &v);

	if (_uses_gain_mapping) {
		v = gain_to_slider_position_with_max (dB_to_coefficient (v), Config->get_max_gain());
	} else {
		double dummy = 0.0;
		model_to_view_coord (dummy, v);
	}

	return v;
}

/** Start dragging a single point, possibly adding others if the supplied point is selected and there
 *  are other selected points.
 *
 *  @param cp Point to drag.
 *  @param x Initial x position (units).
 *  @param fraction Initial y position (as a fraction of the track height, where 0 is the bottom and 1 the top)
 */
void
AutomationLine::start_drag_single (ControlPoint* cp, double x, float fraction)
{
	trackview.editor().session()->begin_reversible_command (_("automation event move"));
	trackview.editor().session()->add_command (
		new MementoCommand<AutomationList> (memento_command_binder(), &get_state(), 0)
		);

	_drag_points.clear ();
	_drag_points.push_back (cp);

	if (cp->get_selected ()) {
		for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {
			if (*i != cp && (*i)->get_selected()) {
				_drag_points.push_back (*i);
			}
		}
	}

	start_drag_common (x, fraction);
}

/** Start dragging a line vertically (with no change in x)
 *  @param i1 Control point index of the `left' point on the line.
 *  @param i2 Control point index of the `right' point on the line.
 *  @param fraction Initial y position (as a fraction of the track height, where 0 is the bottom and 1 the top)
 */
void
AutomationLine::start_drag_line (uint32_t i1, uint32_t i2, float fraction)
{
	trackview.editor().session()->begin_reversible_command (_("automation range move"));
	trackview.editor().session()->add_command (
		new MementoCommand<AutomationList> (memento_command_binder (), &get_state(), 0)
		);

	_drag_points.clear ();
	for (uint32_t i = i1; i <= i2; i++) {
		_drag_points.push_back (nth (i));
	}

	start_drag_common (0, fraction);
}

/** Start dragging multiple points (with no change in x)
 *  @param cp Points to drag.
 *  @param fraction Initial y position (as a fraction of the track height, where 0 is the bottom and 1 the top)
 */
void
AutomationLine::start_drag_multiple (list<ControlPoint*> cp, float fraction, XMLNode* state)
{
	trackview.editor().session()->begin_reversible_command (_("automation range move"));
	trackview.editor().session()->add_command (
		new MementoCommand<AutomationList> (memento_command_binder(), state, 0)
		);

	_drag_points = cp;
	start_drag_common (0, fraction);
}


struct ControlPointSorter
{
	bool operator() (ControlPoint const * a, ControlPoint const * b) {
		return a->get_x() < b->get_x();
	}
};

/** Common parts of starting a drag.
 *  @param x Starting x position in units, or 0 if x is being ignored.
 *  @param fraction Starting y position (as a fraction of the track height, where 0 is the bottom and 1 the top)
 */
void
AutomationLine::start_drag_common (double x, float fraction)
{
	_drag_x = x;
	_drag_distance = 0;
	_last_drag_fraction = fraction;
	_drag_had_movement = false;
	did_push = false;

	_drag_points.sort (ControlPointSorter ());

	/* find the additional points that will be dragged when the user is holding
	   the "push" modifier
	*/

	uint32_t i = _drag_points.back()->view_index () + 1;
	ControlPoint* p = 0;
	_push_points.clear ();
	while ((p = nth (i)) != 0 && p->can_slide()) {
		_push_points.push_back (p);
		++i;
	}
}

/** Should be called to indicate motion during a drag.
 *  @param x New x position of the drag in canvas units, or undefined if ignore_x == true.
 *  @param fraction New y fraction.
 *  @return x position and y fraction that were actually used (once clamped).
 */
pair<double, float>
AutomationLine::drag_motion (double const x, float fraction, bool ignore_x, bool with_push)
{
	/* setup the points that are to be moved this time round */
	list<ControlPoint*> points = _drag_points;
	if (with_push) {
		copy (_push_points.begin(), _push_points.end(), back_inserter (points));
		points.sort (ControlPointSorter ());
	}

	double dx = ignore_x ? 0 : (x - _drag_x);
	double dy = fraction - _last_drag_fraction;

	for (list<ControlPoint*>::iterator i = points.begin(); i != points.end(); ++i) {
		/* Find the points that aren't being moved before and after
		   this one on the control_points list
		*/

		ControlPoint* before = 0;
		ControlPoint* after = 0;

		ControlPoint* last = 0;
		for (vector<ControlPoint*>::iterator j = control_points.begin(); j != control_points.end(); ++j) {

			if (*j == *i) {
				
				before = last;
				
				vector<ControlPoint*>::iterator k = j;

				/* Next point */
				++k;

				/* Now move past any points that are being moved this time */
				while (find (points.begin(), points.end(), *k) != points.end() && k != control_points.end ()) {
					++k;
				}
				
				if (k != control_points.end()) {
					after = *k;
				}
				break;
			}

			if (find (points.begin(), points.end(), *j) == points.end ()) {
				/* This point isn't being moved, so it's the `last' point we've seen */
				last = *j;
			}
		}

		/* Clamp dx for this point */
		double const before_x = before ? before->get_x() : 0;
		double const after_x = after ? after->get_x() : DBL_MAX;

		double tx = (*i)->get_x() + dx;
		tx = max (tx, before_x);
		tx = min (tx, after_x);
		dx = tx - (*i)->get_x ();
	}

	/* clamp y */
	for (list<ControlPoint*>::iterator i = points.begin(); i != points.end(); ++i) {
		double const y = ((_height - (*i)->get_y()) / _height) + dy;
		if (y < 0) {
			dy -= y;
		}
		if (y > 1) {
			dy -= (y - 1);
		}
	}

	pair<double, float> const clamped (_drag_x + dx, _last_drag_fraction + dy);
	_drag_distance += dx;
	_drag_x += dx;
	_last_drag_fraction = fraction;

	for (list<ControlPoint*>::iterator i = _drag_points.begin(); i != _drag_points.end(); ++i) {
		(*i)->move_to ((*i)->get_x() + dx, (*i)->get_y() - _height * dy, ControlPoint::Full);
		reset_line_coords (**i);
	}

	if (with_push) {
		/* move push points, preserving their y */
		for (list<ControlPoint*>::iterator i = _push_points.begin(); i != _push_points.end(); ++i) {
			(*i)->move_to ((*i)->get_x() + dx, (*i)->get_y(), ControlPoint::Full);
			reset_line_coords (**i);
		}
	}

	if (line_points.size() > 1) {
		line->property_points() = line_points;
	}

	_drag_had_movement = true;
	did_push = with_push;

	return clamped;
}

/** Should be called to indicate the end of a drag */
void
AutomationLine::end_drag ()
{
	if (!_drag_had_movement) {
		return;
	}

	alist->freeze ();

	/* set up the points that were moved this time round */
	list<ControlPoint*> points = _drag_points;
	if (did_push) {
		copy (_push_points.begin(), _push_points.end(), back_inserter (points));
		points.sort (ControlPointSorter ());
	}

	sync_model_with_view_points (points, trackview.editor().unit_to_frame (_drag_distance));

	alist->thaw ();

	update_pending = false;

	trackview.editor().session()->add_command (
		new MementoCommand<AutomationList>(memento_command_binder (), 0, &alist->get_state())
		);

	trackview.editor().session()->set_dirty ();
	did_push = false;
}

void
AutomationLine::sync_model_with_view_point (ControlPoint& cp, framecnt_t distance)
{
	/* find out where the visual control point is.
	   initial results are in canvas units. ask the
	   line to convert them to something relevant.
	*/

	double view_x = cp.get_x();
	double view_y = 1.0 - (cp.get_y() / _height);

	/* if xval has not changed, set it directly from the model to avoid rounding errors */

	if (view_x == trackview.editor().frame_to_unit_unrounded (_time_converter->to ((*cp.model())->when)) - _offset) {
		view_x = (*cp.model())->when - _offset;
	} else {
		view_x = trackview.editor().unit_to_frame (view_x);
		view_x = _time_converter->from (view_x + _offset);
	}

	update_pending = true;

	view_to_model_coord_y (view_y);

	alist->modify (cp.model(), view_x, view_y);

	if (did_push) {
		/* move all points after cp by the same distance */
		alist->slide (cp.model()++, _time_converter->from (distance));
	}
}

bool
AutomationLine::control_points_adjacent (double xval, uint32_t & before, uint32_t& after)
{
	ControlPoint *bcp = 0;
	ControlPoint *acp = 0;
	double unit_xval;

	unit_xval = trackview.editor().frame_to_unit_unrounded (xval);

	for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {

		if ((*i)->get_x() <= unit_xval) {

			if (!bcp || (*i)->get_x() > bcp->get_x()) {
				bcp = *i;
				before = bcp->view_index();
			}

		} else if ((*i)->get_x() > unit_xval) {
			acp = *i;
			after = acp->view_index();
			break;
		}
	}

	return bcp && acp;
}

bool
AutomationLine::is_last_point (ControlPoint& cp)
{
	// If the list is not empty, and the point is the last point in the list

	if (alist->empty()) {
		return false;
	}

	AutomationList::const_iterator i = alist->end();
	--i;

	if (cp.model() == i) {
		return true;
	}

	return false;
}

bool
AutomationLine::is_first_point (ControlPoint& cp)
{
	// If the list is not empty, and the point is the first point in the list

	if (!alist->empty() && cp.model() == alist->begin()) {
		return true;
	}

	return false;
}

// This is copied into AudioRegionGainLine
void
AutomationLine::remove_point (ControlPoint& cp)
{
	trackview.editor().session()->begin_reversible_command (_("remove control point"));
	XMLNode &before = alist->get_state();

	alist->erase (cp.model());
	
	trackview.editor().session()->add_command(
		new MementoCommand<AutomationList> (memento_command_binder (), &before, &alist->get_state())
		);

	trackview.editor().session()->commit_reversible_command ();
	trackview.editor().session()->set_dirty ();
}

/** Get selectable points within an area.
 *  @param start Start position in session frames.
 *  @param end End position in session frames.
 *  @param bot Bottom y range, as a fraction of line height, where 0 is the bottom of the line.
 *  @param top Top y range, as a fraction of line height, where 0 is the bottom of the line.
 *  @param result Filled in with selectable things; in this case, ControlPoints.
 */
void
AutomationLine::get_selectables (
	framepos_t start, framepos_t end, double botfrac, double topfrac, list<Selectable*>& results
	)
{
	/* convert fractions to display coordinates with 0 at the top of the track */
	double const bot_track = (1 - topfrac) * trackview.current_height ();
	double const top_track = (1 - botfrac) * trackview.current_height ();

	for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {
		double const model_when = (*(*i)->model())->when;

		/* model_when is relative to the start of the source, so we just need to add on the origin_b here
		   (as it is the session frame position of the start of the source)
		*/
		
		framepos_t const session_frames_when = _time_converter->to (model_when) + _time_converter->origin_b ();

		if (session_frames_when >= start && session_frames_when <= end && (*i)->get_y() >= bot_track && (*i)->get_y() <= top_track) {
			results.push_back (*i);
		}
	}
}

void
AutomationLine::get_inverted_selectables (Selection&, list<Selectable*>& /*results*/)
{
	// hmmm ....
}

void
AutomationLine::set_selected_points (PointSelection const & points)
{
	for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {
		(*i)->set_selected (false);
	}

	for (PointSelection::const_iterator i = points.begin(); i != points.end(); ++i) {
		(*i)->set_selected (true);
	}

	set_colors ();
}

void AutomationLine::set_colors ()
{
	set_line_color (ARDOUR_UI::config()->canvasvar_AutomationLine.get());
	for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {
		(*i)->set_color ();
	}
}

void
AutomationLine::list_changed ()
{
	queue_reset ();
}

void
AutomationLine::reset_callback (const Evoral::ControlList& events)
{
	uint32_t vp = 0;
	uint32_t pi = 0;
	uint32_t np;

	if (events.empty()) {
		for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {
			delete *i;
		}
		control_points.clear ();
		line->hide();
		return;
	}

	/* hide all existing points, and the line */

	for (vector<ControlPoint*>::iterator i = control_points.begin(); i != control_points.end(); ++i) {
		(*i)->hide();
	}

	line->hide ();
	np = events.size();

	Evoral::ControlList& e = const_cast<Evoral::ControlList&> (events);
	
	for (AutomationList::iterator ai = e.begin(); ai != e.end(); ++ai, ++pi) {

		double tx = (*ai)->when;
		double ty = (*ai)->value;

		/* convert from model coordinates to canonical view coordinates */

		model_to_view_coord (tx, ty);

		if (std::isnan (tx) || std::isnan (ty)) {
			warning << string_compose (_("Ignoring illegal points on AutomationLine \"%1\""),
						   _name) << endmsg;
			continue;
		}
		
		if (tx >= max_framepos || tx < 0 || tx >= _maximum_time) {
			continue;
		}
		
		/* convert x-coordinate to a canvas unit coordinate (this takes
		 * zoom and scroll into account).
		 */
			
		tx = trackview.editor().frame_to_unit_unrounded (tx);
		
		/* convert from canonical view height (0..1.0) to actual
		 * height coordinates (using X11's top-left rooted system)
		 */

		ty = _height - (ty * _height);

		add_visible_control_point (vp, pi, tx, ty, ai, np);
		vp++;
	}

	/* discard extra CP's to avoid confusing ourselves */

	while (control_points.size() > vp) {
		ControlPoint* cp = control_points.back();
		control_points.pop_back ();
		delete cp;
	}

	if (!terminal_points_can_slide) {
		control_points.back()->set_can_slide(false);
	}

	if (vp > 1) {

		/* reset the line coordinates given to the CanvasLine */

		while (line_points.size() < vp) {
			line_points.push_back (Art::Point (0,0));
		}

		while (line_points.size() > vp) {
			line_points.pop_back ();
		}

		for (uint32_t n = 0; n < vp; ++n) {
			line_points[n].set_x (control_points[n]->get_x());
			line_points[n].set_y (control_points[n]->get_y());
		}

		line->property_points() = line_points;

		if (_visible && alist->interpolation() != AutomationList::Discrete) {
			line->show();
		}
	}

	set_selected_points (trackview.editor().get_selection().points);
}

void
AutomationLine::reset ()
{
	update_pending = false;

	if (no_draw) {
		return;
	}

	alist->apply_to_points (*this, &AutomationLine::reset_callback);
}

void
AutomationLine::clear ()
{
	/* parent must create and commit command */
	XMLNode &before = alist->get_state();
	alist->clear();

	trackview.editor().session()->add_command (
		new MementoCommand<AutomationList> (memento_command_binder (), &before, &alist->get_state())
		);
}

void
AutomationLine::change_model (AutomationList::iterator /*i*/, double /*x*/, double /*y*/)
{
}

void
AutomationLine::set_list (boost::shared_ptr<ARDOUR::AutomationList> list)
{
	alist = list;
	queue_reset ();
	connect_to_list ();
}

void
AutomationLine::add_visibility (VisibleAspects va)
{
	_visible = VisibleAspects (_visible | va);
	show ();
}

void
AutomationLine::set_visibility (VisibleAspects va)
{
	_visible = va;
	show ();
}

void
AutomationLine::remove_visibility (VisibleAspects va)
{
	_visible = VisibleAspects (_visible & ~va);
	show ();
}

void
AutomationLine::track_entered()
{
	if (alist->interpolation() != AutomationList::Discrete) {
		add_visibility (ControlPoints);
	}
}

void
AutomationLine::track_exited()
{
	if (alist->interpolation() != AutomationList::Discrete) {
		remove_visibility (ControlPoints);
	}
}

XMLNode &
AutomationLine::get_state (void)
{
	/* function as a proxy for the model */
	return alist->get_state();
}

int
AutomationLine::set_state (const XMLNode &node, int version)
{
	/* function as a proxy for the model */
	return alist->set_state (node, version);
}

void
AutomationLine::view_to_model_coord (double& x, double& y) const
{
	x = _time_converter->from (x);
	view_to_model_coord_y (y);
}

void
AutomationLine::view_to_model_coord_y (double& y) const
{
	/* TODO: This should be more generic ... */
	if (alist->parameter().type() == GainAutomation ||
	    alist->parameter().type() == EnvelopeAutomation) {
		y = slider_position_to_gain_with_max (y, Config->get_max_gain());
		y = max (0.0, y);
		y = min (2.0, y);
	} else if (alist->parameter().type() == PanAzimuthAutomation ||
                   alist->parameter().type() == PanElevationAutomation ||
                   alist->parameter().type() == PanWidthAutomation) {
		y = 1.0 - y;
	} else if (alist->parameter().type() == PluginAutomation) {
		y = y * (double)(alist->get_max_y()- alist->get_min_y()) + alist->get_min_y();
	} else {
		y = rint (y * alist->parameter().max());
	}
}

void
AutomationLine::model_to_view_coord (double& x, double& y) const
{
	/* TODO: This should be more generic ... */
	if (alist->parameter().type() == GainAutomation ||
	    alist->parameter().type() == EnvelopeAutomation) {
		y = gain_to_slider_position_with_max (y, Config->get_max_gain());
	} else if (alist->parameter().type() == PanAzimuthAutomation ||
                   alist->parameter().type() == PanElevationAutomation ||
                   alist->parameter().type() == PanWidthAutomation) {
		// vertical coordinate axis reversal
		y = 1.0 - y;
	} else if (alist->parameter().type() == PluginAutomation) {
		y = (y - alist->get_min_y()) / (double)(alist->get_max_y()- alist->get_min_y());
	} else {
		y = y / (double)alist->parameter().max(); /* ... like this */
	}

	x = _time_converter->to (x) - _offset;
}

/** Called when our list has announced that its interpolation style has changed */
void
AutomationLine::interpolation_changed (AutomationList::InterpolationStyle style)
{
	if (style == AutomationList::Discrete) {
		set_visibility (ControlPoints);
		line->hide();
	} else {
		set_visibility (Line);
	}
}

void
AutomationLine::add_visible_control_point (uint32_t view_index, uint32_t pi, double tx, double ty, 
					   AutomationList::iterator model, uint32_t npoints)
{
	ControlPoint::ShapeType shape;

	if (view_index >= control_points.size()) {

		/* make sure we have enough control points */

		ControlPoint* ncp = new ControlPoint (*this);
		ncp->set_size (control_point_box_size ());

		control_points.push_back (ncp);
	}

	if (!terminal_points_can_slide) {
		if (pi == 0) {
			control_points[view_index]->set_can_slide (false);
			if (tx == 0) {
				shape = ControlPoint::Start;
			} else {
				shape = ControlPoint::Full;
			}
		} else if (pi == npoints - 1) {
			control_points[view_index]->set_can_slide (false);
			shape = ControlPoint::End;
		} else {
			control_points[view_index]->set_can_slide (true);
			shape = ControlPoint::Full;
		}
	} else {
		control_points[view_index]->set_can_slide (true);
		shape = ControlPoint::Full;
	}

	control_points[view_index]->reset (tx, ty, model, view_index, shape);

	/* finally, control visibility */

	if (_visible & ControlPoints) {
		control_points[view_index]->show ();
		control_points[view_index]->set_visible (true);
	} else {
		control_points[view_index]->set_visible (false);
	}
}

void
AutomationLine::connect_to_list ()
{
	_list_connections.drop_connections ();

	alist->StateChanged.connect (_list_connections, invalidator (*this), boost::bind (&AutomationLine::list_changed, this), gui_context());

	alist->InterpolationChanged.connect (
		_list_connections, invalidator (*this), boost::bind (&AutomationLine::interpolation_changed, this, _1), gui_context()
		);
}

MementoCommandBinder<AutomationList>*
AutomationLine::memento_command_binder ()
{
	return new SimpleMementoCommandBinder<AutomationList> (*alist.get());
}

/** Set the maximum time that points on this line can be at, relative
 *  to the start of the track or region that it is on.
 */
void
AutomationLine::set_maximum_time (framecnt_t t)
{
	if (_maximum_time == t) {
		return;
	}

	_maximum_time = t;
	reset ();
}


/** @return min and max x positions of points that are in the list, in session frames */
pair<framepos_t, framepos_t>
AutomationLine::get_point_x_range () const
{
	pair<framepos_t, framepos_t> r (max_framepos, 0);

	for (AutomationList::const_iterator i = the_list()->begin(); i != the_list()->end(); ++i) {
		r.first = min (r.first, session_position (i));
		r.second = max (r.second, session_position (i));
	}

	return r;
}

framepos_t
AutomationLine::session_position (AutomationList::const_iterator p) const
{
	return _time_converter->to ((*p)->when) + _offset + _time_converter->origin_b ();
}

void
AutomationLine::set_offset (framepos_t off)
{
	if (_offset == off) {
		return;
	}

	_offset = off;
	reset ();
}
