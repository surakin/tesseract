-- The local player of the 3D call room: walking, turning, looking and the first-person camera.
--
-- Attached to the camera entity by the host (behavior 'local_player'). The host sends what the
-- user is doing and gets back where the player stands:
--
--   in  'input'  [forward, strafe, turn, look_dx, look_dy, jump]
--                forward: +1 W/Up, -1 S/Down.  strafe: +1 left (A), -1 right (D).
--                turn: +1 left arrow, -1 right arrow.  look_dx/look_dy: radians dragged since the
--                last event (dx > 0 turns right, dy > 0 looks down); they add up until used.
--                jump: 1 while Space is held.
--   in  'reset'  [x, z, yaw, pitch]   put the player somewhere (a teleport)
--   in  'nudge'  [dx, dz]             slide this far over a moment (another player's push)
--   out 'pose'   [x, z, yaw, pitch, speed, feet_y, eye_y]   once per tick; speed is 0 when standing;
--                feet_y is the height of the feet in the world (the ground under the player, plus a
--                jump) and eye_y the eye's, metres
-- The player is an engine character (a capsule that falls, stands on the colliders of the world,
-- climbs small steps and slides along walls) on the camera entity, its centre at the eye: the
-- engine moves the entity, this script only says where to walk and when to jump.
--
-- params (numbers, from the host), all optional: x, z, yaw, pitch (the start, default 0);  half_extent (the walkable area
-- is the square of this half side around the origin, default 80) and eye_height (metres above the
-- ground, default 1.06).
--
-- The start and every teleport put the capsule on the ground found by a ray cast down from above the place, so
-- the world's surface can have any shape; a world with no collider under the start waits GROUND_WAIT seconds, then
-- starts the player at height 0 (a world without collision marks: the player falls and starts again).
--
-- Axes: +Y up, metres. Yaw 0 faces +Z and grows counter-clockwise seen from above, so the heading
-- is (sin yaw, cos yaw) on x/z. The camera looks down its own -Z, hence the extra half turn.

local SKIN = 0.02 -- m: the character controller's gap between the capsule and the ground
local WALK_SPEED = 2.8 -- m/s
local TURN_RATE = math.pi / 2 -- rad/s from the left and right keys
local JUMP_SPEED = 3.5 -- m/s upward: about 0.6 m high
local RADIUS = 0.25 -- m, the capsule
local STEP_HEIGHT = 0.25 -- m, the highest ledge it walks onto
local DROP = 0.1 -- m: the player starts this far above the ground and lands (a capsule put exactly on a
-- surface does not walk)
local RAY_TOP = 100 -- m: the ray that finds the ground under a place starts this high and points down
local RAY_LENGTH = 200
local GROUND_WAIT = 2 -- s: a world with no ground under the start (no collision marks) starts the player at height 0 after this
local FALL_LIMIT = -20 -- m: below this the player fell out of the world and starts again
local NUDGE_TIME = 0.15 -- s: another player's push is walked off over this long
local NUDGE_MAX = 0.5 -- m: the most that waits to be walked off
local MAX_PITCH = math.rad(80)
local MAX_DT = 0.1 -- a stalled frame does not throw the player across the meadow
local TWO_PI = 2 * math.pi

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- A yaw folded into [0, 2 pi).
local function wrap(a)
  a = a % TWO_PI
  if a >= TWO_PI then return 0 end
  return a
end

-- The feet's height for an eye at `eye_y`: the standing capsule's eye rests eye_height + SKIN above them.
local function feet_of(self, eye_y)
  return eye_y - self.eye_height - SKIN
end

-- Where the player stands: the horizontal place is the capsule's, kept inside the walkable square.
local function place(self, x, z, yaw, pitch)
  self.x = clamp(x, -self.half_extent, self.half_extent)
  self.z = clamp(z, -self.half_extent, self.half_extent)
  self.yaw = wrap(yaw)
  self.pitch = clamp(pitch, -MAX_PITCH, MAX_PITCH)
end

-- The camera looks where the player faces.
local function aim(self)
  transform.set_rotation(self.entity,
    quat.mul(quat.from_axis_angle(vec3.new(0, 1, 0), self.yaw + math.pi),
             quat.from_axis_angle(vec3.new(1, 0, 0), self.pitch)))
end

-- The height of the ground under (x, z): the first collider a ray from far above hits, or nil.
local function ground_height(x, z)
  local hit = physics.raycast(vec3.new(x, RAY_TOP, z), vec3.new(0, -1, 0), RAY_LENGTH, nil)
  return hit and hit.point.y or nil
end

-- Where the capsule's centre goes to stand at the player's place: the eye height above the ground
-- (and DROP above that, to land), or nil while there is no ground to find yet.
local function standing_height(self)
  local ground = ground_height(self.x, self.z)
  return ground and ground + self.eye_height + DROP or nil
end

-- Puts the player back at its place (a teleport). Returns the height of the capsule's centre.
local function teleport(self)
  local y = standing_height(self) or self.eye_height + DROP
  character.set_position(self.entity, vec3.new(self.x, y, self.z))
  return y
end

-- Makes the character, once the ground under the start can be found (the level is spawned a little
-- after this script starts), or when waiting for it is pointless.
local function try_place(self, dt)
  self.waited = self.waited + dt
  local y = standing_height(self)
  if y == nil and self.waited > GROUND_WAIT then
    y = self.eye_height + DROP
  end
  if y == nil then
    return false
  end
  self.eye_y = y
  transform.set_position(self.entity, vec3.new(self.x, y, self.z))
  character.create(self.entity, {
    radius = RADIUS,
    height = 2 * self.eye_height,
    step_height = STEP_HEIGHT,
  })
  self.placed = true
  return true
end

behavior('local_player', {
  on_create = function(self)
    local p = self.params
    self.half_extent = p.half_extent or 80
    self.eye_height = p.eye_height or 1.06
    self.forward, self.strafe, self.turn, self.jump = 0, 0, 0, false
    self.look_dx, self.look_dy = 0, 0
    place(self, p.x or 0, p.z or 0, p.yaw or 0, p.pitch or 0)
    self.start_x, self.start_z = self.x, self.z

    self.placed, self.waited, self.eye_y = false, 0, self.eye_height

    event.on('input', function(me, v)
      me.forward, me.strafe, me.turn = v[1], v[2], v[3]
      me.jump = v[6] ~= nil and v[6] > 0.5
      me.look_dx = me.look_dx + v[4]
      me.look_dy = me.look_dy + v[5]
    end)
    self.nudge_x, self.nudge_z = 0, 0
    event.on('nudge', function(me, v)
      -- [dx, dz]: where another player wants us to move, in metres.
      me.nudge_x = clamp(me.nudge_x + v[1], -NUDGE_MAX, NUDGE_MAX)
      me.nudge_z = clamp(me.nudge_z + v[2], -NUDGE_MAX, NUDGE_MAX)
    end)
    event.on('reset', function(me, v)
      place(me, v[1], v[2], v[3], v[4])
      me.look_dx, me.look_dy = 0, 0
      if me.placed then
        me.eye_y = teleport(me)
      end
    end)
  end,

  on_update = function(self, dt)
    dt = math.min(dt, MAX_DT)

    -- The character is made once the ground is there and exists from a later tick; until then the
    -- player stands at the start.
    if not self.placed then
      try_place(self, dt)
    end
    if not self.placed or not character.exists(self.entity) then
      aim(self)
      event.emit('pose', { self.x, self.z, self.yaw, self.pitch, 0, feet_of(self, self.eye_y), self.eye_y })
      return
    end

    -- Where the engine put the capsule at the last step.
    local at = transform.position(self.entity)
    if at.y < FALL_LIMIT then
      self.x, self.z = self.start_x, self.start_z -- fell out of the world: start again
      at = vec3.new(self.x, teleport(self), self.z)
    end

    -- Turn and look first, then walk along the new heading.
    self.yaw = wrap(self.yaw - self.look_dx + self.turn * TURN_RATE * dt)
    self.pitch = clamp(self.pitch - self.look_dy, -MAX_PITCH, MAX_PITCH)
    self.look_dx, self.look_dy = 0, 0

    local forward, side = self.forward, self.strafe
    local len = math.sqrt(forward * forward + side * side)
    if len > 1 then
      forward, side = forward / len, side / len
    end
    local speed = WALK_SPEED * math.min(len, 1)

    -- The heading turned a quarter turn the way yaw grows is "left".
    local sy, cy = math.sin(self.yaw), math.cos(self.yaw)
    local vx = (sy * forward + cy * side) * WALK_SPEED
    local vz = (cy * forward - sy * side) * WALK_SPEED
    -- A push from another player is walked off over NUDGE_TIME, on top of the walking.
    local step = math.min(dt / NUDGE_TIME, 1)
    local px, pz = self.nudge_x * step, self.nudge_z * step
    self.nudge_x, self.nudge_z = self.nudge_x - px, self.nudge_z - pz
    if dt > 0 then
      vx, vz = vx + px / dt, vz + pz / dt
    end
    -- The edge of the walkable square holds the player in.
    local edge = self.half_extent - RADIUS
    if (at.x >= edge and vx > 0) or (at.x <= -edge and vx < 0) then vx = 0 end
    if (at.z >= edge and vz > 0) or (at.z <= -edge and vz < 0) then vz = 0 end
    character.move(self.entity, vec3.new(vx, 0, vz))

    if self.jump and character.is_grounded(self.entity) then
      character.jump(self.entity, JUMP_SPEED)
    end

    self.x, self.z = at.x, at.z
    aim(self)

    event.emit('pose', { self.x, self.z, self.yaw, self.pitch, speed, feet_of(self, at.y), at.y })
  end,
})
