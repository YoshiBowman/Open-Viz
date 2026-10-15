// Turns a mode's channel values into what a renderer needs: which geometries turn and by how
// much, and for every beam its intensity, colour, strobe rate and angle.
//
// Which channel drives which beam follows GDTF's "trickle down" rule: a channel linked to a
// geometry applies to that geometry and everything below it. A dimmer on "Head" therefore dims
// every beam under the head, while a ColorAdd_R on a referenced "Pixel" colours one cell.
//
// Channels without a DMX address (virtual channels) describe something the console does, such as
// a virtual dimmer it multiplies into the colour channels before sending them. Their effect is
// already in the DMX that arrives, so they are ignored for intensity and colour. They still give
// the rest position of a manually focused pan, tilt or zoom.
//
// Read from the channel function that is active:
//   Pan, Tilt                    angle in degrees
//   XYZ_X, XYZ_Y, XYZ_Z          the geometry slid along its own axis, in metres: a winch, a hoist
//   Rot_X, Rot_Y, Rot_Z          the geometry turned about its own axis, in degrees: a turntable
//   Dimmer                       level, 0-1 across the function's physical range
//   Shutter<n>                   0 closed to 1 open
//   Shutter<n>Strobe...          flash rate in Hz
//   Zoom                         beam angle in degrees
//   ColorAdd_*                   additive emitters (the linked Emitter's colour, else a default)
//   ColorRGB_Red/Green/Blue      direct RGB
//   Shutter<n>Strobe...          flash rate in Hz, and by the rest of the name how a flash looks:
//                                Pulse (short), PulseClose (snaps open, fades), PulseOpen (fades up,
//                                snaps shut), Random and the random forms of each
//   ColorSub_C/M/Y               subtractive flags: toward the colour of the Filter the function
//                                names, if it has one, else toward ideal cyan, magenta, yellow
//   Color<n>                     colour wheel: the slot the active channel set selects
//   Color<n>WheelSpin            the wheel turning: the beam runs through its slots, degrees a second
//   Color<n>WheelIndex           the wheel held anywhere, also between two slots, in degrees
//   CTO, CTC, CTB                colour temperature, when the physical range is in kelvin
//   Gobo<n>, AnimationWheel<n>   the wheel slot the active channel set selects, for gobo wheels 1 and
//                                2 and animation wheel 1; with ...SelectSpin and ...SelectShake also
//                                how fast it turns or shakes
//   Gobo<n>WheelSpin, ...WheelIndex, ...WheelShake    the wheel itself turning, held, shaking
//   Gobo<n>Pos, ...PosRotate, ...PosShake             the slot turned in its holder, turning, shaking
//   Prism<n>, Prism<n>SelectSpin, Prism<n>Pos, Prism<n>PosRotate    prisms 1 and 2, likewise
//   Iris                         the share of the beam's width left open, 1 = open
//   IrisStrobe, IrisPulseClose, IrisPulseOpen and their random forms   the iris pulsing, in Hz
//   Frost<n>                     0 to 1; the strongest of several counts
//   Focus<n>                     how far the channel is from where the file's default leaves it:
//                                GDTF does not say at what distance a focus value is sharp
//   Blade<n>A, Blade<n>B, Blade<n>Rot, ShaperRot      framing shutters 1 to 4 (top, right, bottom, left)
//
// Not modelled yet: continuous pan/tilt rotation, wheels driven by sound or at random, frost and
// gobo macros and pulses, a third gobo wheel, relations and DMX profiles.
#pragma once

#include <cstddef>
#include <vector>

#include "vizcore/evaluate.h"

namespace viz {

struct ColorRGB {
    double r = 1.0;
    double g = 1.0;
    double b = 1.0;
};

// CIE 1931 xyY to linear sRGB (D65). Y is on GDTF's 0-100 scale. The result is not clamped:
// colours outside sRGB have negative components.
VIZCORE_API ColorRGB to_linear_srgb(const ColorCIE& color);

// A black-body colour for a colour temperature, linear sRGB with the largest component at 1.
// An approximation, good enough to make a CTO look warm.
VIZCORE_API ColorRGB color_temperature_rgb(double kelvin);

// How one flash of a strobe looks.
enum class StrobeShape { Square, Pulse, RampDown, RampUp, Random, RandomPulse, RandomRampDown, RandomRampUp };

// How open the shutter is, 0 to 1, at `phase` (0 to 1) of a strobe cycle. `chance` is a number from
// 0 to 1 drawn anew for each cycle; the random shapes flash in about half of their cycles.
VIZCORE_API double strobe_level(StrobeShape shape, double phase, double chance);

// The colour of a wheel `turns` of a revolution round from its first slot. Each slot has an equal
// share of the circle; across the last tenth of a slot the beam straddles two and their colours mix.
VIZCORE_API ColorRGB wheel_color_at(const Wheel& wheel, double turns);

// A wheel slot with a picture, sitting in the beam: a gobo, or an animation wheel.
struct GateLayer {
    const Wheel* wheel = nullptr;             // null: this wheel has nothing in the beam
    int slot = 0;                             // the slot in the beam, counted from 1; 0 while the wheel itself turns or is held
    double degrees = 0.0;                     // the slot turned in its holder
    double degrees_per_second = 0.0;          // the slot turning in its holder
    double shake_hz = 0.0;                    // the slot shaking about its place
    double wheel_degrees = 0.0;               // slot 0: the wheel held at this angle; 0 has its first slot in the beam
    double wheel_degrees_per_second = 0.0;    // slot 0: the wheel turning, its slots passing through the beam
};

struct PrismState {
    const WheelSlot* slot = nullptr;    // the prism in the beam; prism_images() says where it puts its copies
    double degrees = 0.0;
    double degrees_per_second = 0.0;
};

// One framing shutter. Either both ends of its edge are driven (a and b), or its middle and its angle.
struct BladeState {
    bool present = false;
    bool two_ends = false;
    double a = 0.0;          // how far in: 0 at the rim of the beam, 0.5 at its centre, 1 at the far rim
    double b = 0.0;          // two_ends: the other end, likewise
    double degrees = 0.0;    // otherwise: the edge's own rotation
};

constexpr int kGateLayers = 3;    // gobo wheel 1, gobo wheel 2, animation wheel 1
constexpr int kPrisms = 2;

struct BeamState {
    int geometry = -1;           // index into FixtureModel::geometry()
    double intensity = 0.0;      // 0-1 of the beam's luminous flux: dimmers x shutter x colour level
    double strobe_hz = 0.0;      // 0 = steady, otherwise the output flashes at this rate
    StrobeShape strobe_shape = StrobeShape::Square;
    // A colour wheel that is turning or held between slots. Its colour is not in `color`: multiply
    // by wheel_color_at(*wheel, turns), where turns is wheel_degrees / 360 plus, when it spins, the
    // revolutions made so far at wheel_degrees_per_second.
    const Wheel* wheel = nullptr;
    double wheel_degrees = 0.0;
    double wheel_degrees_per_second = 0.0;
    ColorRGB color;              // linear sRGB with the largest component at 1; white if uncoloured
    double beam_angle = 25.0;    // degrees, full angle
    double field_angle = 25.0;

    // What shapes the beam on its way out.
    GateLayer layers[kGateLayers];
    PrismState prisms[kPrisms];
    double iris = 1.0;           // the share of the beam's width the iris leaves open
    double iris_hz = 0.0;        // above 0 the iris pulses at this rate, between `iris` and open
    StrobeShape iris_shape = StrobeShape::Square;
    double frost = 0.0;          // 0 none to 1 full
    double defocus = 0.0;        // 0 sharp to 1 as soft as the focus channel makes it
    BladeState blades[4];        // top, right, bottom, left
    double shaper_degrees = 0.0; // the four blades turned together

    // True when nothing but the lens shapes the beam: no gobo, prism, iris, frost, blade or blur.
    bool gate_open() const;
};

// Which slot of a turning wheel is in the beam, and how far it has still to go: `offset` is -0.5
// as the slot enters, 0 when it is centred and 0.5 as it leaves, in slot spacings.
struct WheelPlace {
    int slot = 1;    // counted from 1
    double offset = 0.0;
};
VIZCORE_API WheelPlace wheel_place_at(const Wheel& wheel, double turns);

// Where a prism puts one of its copies of the beam: its centre, in beam radii from the beam's own
// centre, and how much larger or smaller the copy is across and up.
struct PrismImage {
    double x = 0.0;
    double y = 0.0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    ColorRGB color;
};

// The copies a prism makes. A facet's matrix carries a size and an offset; real files put the
// offset in the third row. A slot that describes no facets is given three, evenly round a ring:
// the file says a prism is there but not what kind.
VIZCORE_API std::vector<PrismImage> prism_images(const WheelSlot& slot);

// The straight edge a framing shutter casts across the beam. Seen along the beam with x to the
// right and y up, and the rim of the beam at distance 1 from its centre, the points where
// x * nx + y * ny > distance are in the blade's shadow. `blade` is 0 top, 1 right, 2 bottom, 3 left.
struct BladeEdge {
    double nx = 0.0;
    double ny = 1.0;
    double distance = 1.0;
};
VIZCORE_API BladeEdge blade_edge(int blade, const BladeState& state, double shaper_degrees);

// How a channel moves a geometry. Pan and Tilt are turns too, kept apart because an MVR may
// invert them per fixture.
enum class Motion { Pan, Tilt, Turn, Slide };

// A geometry that a channel moves: Pan and Tilt, and the XYZ and Rot channels of things that
// travel (a winch's hook, a hoist, a turntable). Everything below the geometry moves with it.
struct AxisState {
    int geometry = -1;                 // index into FixtureModel::geometry()
    Motion motion = Motion::Pan;
    char axis = 'Z';                   // the geometry's own axis it turns about or slides along; pan is Z, tilt X
    double target = 0.0;               // degrees, or metres for a slide
    double range = 0.0;                // degrees (metres) between the ends of the function's physical range
    double center = 0.0;               // middle of that range; an inverted axis mirrors about it
    double real_fade = 0.0;            // seconds for the full range; 0 when the file does not say
    double real_acceleration = 0.0;    // seconds from rest to full speed
};

// Where an axis puts its geometry when it stands at `value` (degrees, or metres for a slide): a
// matrix in the geometry's own frame, to be applied before the geometry's place in its parent.
VIZCORE_API Matrix4 axis_matrix(const AxisState& axis, double value);

struct FixtureState {
    std::vector<AxisState> axes;
    std::vector<BeamState> beams;
    // How hard the fixture's fog or haze machine is working, 0-1: the most of its Fog and Haze
    // channels. 0 for a fixture that has none.
    double fog = 0.0;
};

// A beam as it stands when the fixture is at rest: every channel at its default, the dimmers
// full. For a plot's light levels. In the fixture's own frame, metres; the light leaves the
// origin of `world` along its -Z.
struct RestBeam {
    int geometry = -1;
    Matrix4 world;
    BeamType beam_type = BeamType::Wash;
    double lumens = 0.0;                        // luminous flux at full
    double beam_angle = 0.0, field_angle = 0.0; // degrees, as the beam stands with zoom at its default
    double intensity = 0.0;                     // 0-1: what the shutter and the colour leave of the flux
};
VIZCORE_API std::vector<RestBeam> rest_beams(const FixtureType& fixture, size_t mode_index);

// Beams that are drawn as one. A wash light's head is an array of small lenses, and its file may
// describe every lens as a beam of its own, with its own colour and level; a strobe's panel may be
// forty cells. Beyond a hand's breadth such beams are one beam to the eye, and to draw each on its
// own costs forty times what the picture gains. So lenses that sit side by side in a patch about
// as round as it is wide, shining the same way from the same moving part, are a cluster; and so
// are cells whose light spreads so widely (a field of 100 degrees or more) that it is a glow, not
// a shaft, however they are laid out, in clusters of at most 70 cm. The cells of a batten in a
// row, whose narrow beams stand side by side as a fan, stay beams of their own. A beam that takes
// a gobo, a prism, an iris or blades is never clustered.
//
// Whoever draws a cluster draws one beam from its middle, as wide at the start as the cluster,
// with the members' light added up; each member still glows in its own colour.
struct BeamCluster {
    int leader = -1;              // the member nearest the middle: an index into the model's beams
    std::vector<int> members;     // all of them, the leader among them, in the model's order
    double radius = 0.0;          // metres: of the circle about the cluster's middle that holds every member's lens
};

class VIZCORE_API FixtureModel {
public:
    // `fixture` must outlive the model.
    FixtureModel(const FixtureType& fixture, size_t mode_index);

    bool valid() const { return evaluator_.valid(); }
    const ModeEvaluator& evaluator() const { return evaluator_; }
    const std::vector<GeometryInstance>& geometry() const { return evaluator_.geometry(); }

    // Both counts are fixed for the model; resolve() always fills that many entries, in the same order.
    size_t axis_count() const { return axes_.size(); }
    size_t beam_count() const { return beams_.size(); }
    // Whether the mode has a Fog or Haze channel: the fixture is, or carries, a haze machine.
    bool has_fog() const { return has_fog_; }
    // The beams that are drawn as one (see BeamCluster): only clusters of two or more are listed.
    // `cluster_of` is a beam's cluster, or -1 for a beam that is drawn by itself.
    const std::vector<BeamCluster>& clusters() const { return clusters_; }
    int cluster_of(size_t beam) const { return beam < cluster_of_.size() ? cluster_of_[beam] : -1; }
    // The geometry that what hangs on this fixture rides on, where an MVR does not name one: the
    // deepest part an XYZ or Rot channel moves (a winch's hook, a turntable's top), or -1 when
    // nothing travels. Pan and tilt do not count: what is rigged to a moving head hangs on its base.
    int carrier() const;

    // Writes `level` (0-1) into every dimmer channel of a break-1 frame, for lighting a fixture
    // up without a console: start from evaluator().default_frame() and call this.
    void set_dimmers(std::vector<uint8_t>& frame, double level) const;

    // `values` is what ModeEvaluator::evaluate produced for this model's evaluator.
    // An axis whose channel is currently doing something else (continuous rotation, say) keeps
    // the target already in `out`, so pass the same FixtureState every frame.
    void resolve(const std::vector<ChannelValue>& values, FixtureState& out) const;

private:
    enum class Kind {
        None, Pan, Tilt, Dimmer, Shutter, Strobe, Zoom, Additive, DirectRgb, Subtractive, Wheel, WheelSpin, WheelIndex, Temperature,
        Gate, Prism, Iris, IrisPulse, Frost, Focus, Blade, ShaperRot, Fog, Slide, Turn,
    };
    // What a Gate or Prism function does to its wheel.
    enum Role { Select, SelectSpin, SelectShake, WheelTurn, WheelHold, WheelShake, Pos, PosRotate, PosShake };

    struct FunctionInfo {
        Kind kind = Kind::None;
        int component = 0;             // 0-2 for DirectRgb and Subtractive; a Role for Gate and Prism; A, B or Rot (0-2) for Blade; X, Y or Z (0-2) for Slide and Turn
        int index = 0;                 // which layer, prism or blade
        double reference = 0.0;        // Focus: the physical value at the function's default
        int slot = -1;                 // nearest-wins slot: functions sharing it are the same control
        ColorRGB color;                // Additive: the emitter's colour; Subtractive: the filter's. Largest component 1
        StrobeShape shape = StrobeShape::Square;
        const Wheel* wheel = nullptr;
    };

    struct ChannelInfo {
        bool is_virtual = false;
        std::vector<size_t> first_function;    // flat index of each logical channel's first function
        std::vector<FunctionInfo> functions;   // flat, in file order
    };

    struct AxisBinding {
        int geometry = -1;
        int channel_instance = -1;
        Motion motion = Motion::Pan;
        char axis = 'Z';
    };
    // Whether a function moves its geometry, and how.
    static bool moves(const FunctionInfo& info, Motion& motion, char& axis);

    struct BeamBinding {
        int geometry = -1;
        const BeamProperties* properties = nullptr;
        std::vector<int> channel_instances;    // on the beam or above it, nearest first
    };

    const FunctionInfo* active(int channel_instance, const ChannelValue& value) const;
    void find_clusters();

    ModeEvaluator evaluator_;
    std::vector<ChannelInfo> channels_;    // one per DmxMode channel
    bool has_fog_ = false;
    std::vector<AxisBinding> axes_;
    std::vector<BeamBinding> beams_;
    std::vector<BeamCluster> clusters_;
    std::vector<int> cluster_of_;    // per beam
    int slot_count_ = 0;
};

// Speed- and acceleration-limited movement toward a target, for pan and tilt and for things that
// travel (then read metres for degrees).
class VIZCORE_API MotorAxis {
public:
    // `range` is the travel in degrees that takes `real_fade` seconds from standstill to
    // standstill, of which `real_acceleration` seconds are spent getting up to full speed.
    // A `real_fade` of 0 means the file gave no speed: the axis then follows its target at once.
    void configure(double range, double real_fade, double real_acceleration);
    void reset(double position);

    // Advances by `dt` seconds and returns the new position. Never overshoots a fixed target.
    double step(double target, double dt);

    double position() const { return position_; }
    double velocity() const { return velocity_; }
    double max_speed() const { return max_speed_; }           // degrees per second, 0 = unlimited
    double acceleration() const { return acceleration_; }     // degrees per second squared, 0 = unlimited

private:
    double position_ = 0.0;
    double velocity_ = 0.0;
    double max_speed_ = 0.0;
    double acceleration_ = 0.0;
};

}  // namespace viz
