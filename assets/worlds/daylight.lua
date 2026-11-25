-- The daylight cycle of the 3D call room: a sun and a moon crossing the sky, and the sky, the
-- ambient light and the lights following them.
--
-- Attached by the host (behavior 'daylight') to an entity of its own. One day lasts DAY_SECONDS of
-- UTC wall-clock time, so everybody in a call sees the same hour with nothing sent between them.
-- param clock (number): seconds into the day to show instead of the wall clock; negative or missing
-- follows the wall clock (the tests pin it).
-- The meadow file carries the four entities it moves: the directional lights "SunLight" and
-- "MoonLight" and the discs "Sun" and "Moon". It listens to the player's 'pose' event (see
-- player.lua) to keep the discs on the sky around the camera.
--
-- The sun rises at 06:00 of the day's clock (a quarter of the way), is overhead at noon and sets at
-- 18:00; the moon is always opposite it. Axes: +Y up, the sun's path runs from +X (rising) to -X
-- (setting) and leans toward -Z at noon.

local DAY_SECONDS = 24 * 60 -- one day lasts 24 minutes
local TILT = math.rad(25) -- how far the path leans from straight overhead
local DISC_DISTANCE = 150 -- m from the camera (its far plane is 200 m)

local REFLECTION = 1.0 -- how much of the sky glass and other smooth surfaces mirror (the engine's Fresnel term)
local SUN_INTENSITY = 1.5
local MOON_INTENSITY = 0.25

local DAY_SKY = { 0.45, 0.65, 0.90 }
local NIGHT_SKY = { 0.008, 0.015, 0.05 }
local DUSK_SKY = { 0.90, 0.45, 0.25 }
local NIGHT_AMBIENT = { 0.35, 0.45, 0.80 }
local DAY_AMBIENT = { 1.0, 1.0, 1.0 }
local SUN_NOON = { 1.0, 0.97, 0.90 }
local SUN_HORIZON = { 1.0, 0.55, 0.25 }
local MOON_COLOR = { 0.55, 0.65, 1.0 }

local function mix(a, b, t)
  return { num.lerp(a[1], b[1], t), num.lerp(a[2], b[2], t), num.lerp(a[3], b[3], t) }
end

local function vec(c)
  return vec3.new(c[1], c[2], c[3])
end

-- The unit vector from the camera toward the sun at `day` (0..1 of the day).
local function sun_direction(day)
  local a = 2 * math.pi * (day - 0.25)
  return math.cos(a), math.sin(a) * math.cos(TILT), -math.sin(a) * math.sin(TILT)
end

-- Turns a directional light's entity so it shines along `(dx, dy, dz)` (its -Z axis points there).
local function shine_along(entity, dx, dy, dz)
  -- The rotation taking (0, 0, -1) to d: the axis is their cross product, the angle the arc.
  local cx, cy, cz = 0 * dz - (-1) * dy, (-1) * dx - 0 * dz, 0
  local len = math.sqrt(cx * cx + cy * cy + cz * cz)
  local dot = -dz
  if len < 1e-6 then
    return -- straight along -Z already: the spawned entity's own rotation is that
  end
  transform.set_rotation(entity,
    quat.from_axis_angle(vec3.new(cx / len, cy / len, cz / len), math.acos(num.clamp(dot, -1, 1))))
end

behavior('daylight', {
  on_create = function(self)
    self.camera = { 0, 1, 0 }
    event.on('pose', function(me, v)
      -- [x, z, yaw, pitch, speed, y]: the camera stands at x, y, z.
      me.camera = { v[1], v[6] or 1, v[2] }
    end)
  end,

  on_update = function(self, dt)
    local clock = self.params.clock
    if not clock or clock < 0 then
      clock = time.real()
    end
    local day = (clock % DAY_SECONDS) / DAY_SECONDS
    local sx, sy, sz = sun_direction(day)

    -- The meadow's entities exist once the level is spawned, a little after this script starts.
    if not self.sun then
      self.sun = entity.find('SunLight')
      self.moon = entity.find('MoonLight')
      self.sun_disc = entity.find('Sun')
      self.moon_disc = entity.find('Moon')
    end

    -- The lights shine toward the ground: along minus the way to the body.
    if self.sun then
      shine_along(self.sun, -sx, -sy, -sz)
    end
    if self.moon then
      shine_along(self.moon, sx, sy, sz)
    end

    -- How far the sun is up: 0 at the horizon, 1 near the top; the moon is the opposite.
    local sun_up = num.smoothstep(-0.05, 0.25, sy)
    local moon_up = num.smoothstep(-0.05, 0.25, -sy)
    local warm = num.smoothstep(0.0, 0.45, sy) -- 0 at the horizon (orange), 1 high (white)
    if self.sun then
      light.set(self.sun, { color = vec(mix(SUN_HORIZON, SUN_NOON, warm)), intensity = SUN_INTENSITY * sun_up })
    end
    if self.moon then
      light.set(self.moon, { color = vec(MOON_COLOR), intensity = MOON_INTENSITY * moon_up })
    end

    -- The sky: blue by day, dark by night, orange for a while around sunrise and sunset.
    local daylight = num.smoothstep(-0.1, 0.35, sy)
    local glow = num.clamp(1 - math.abs(sy) / 0.3, 0, 1)
    local sky = mix(mix(NIGHT_SKY, DAY_SKY, daylight), DUSK_SKY, glow * 0.7)
    local ambient = mix(NIGHT_AMBIENT, DAY_AMBIENT, daylight)
    environment.set({
      sky_color = vec(sky),
      ambient_color = vec(ambient),
      ambient_intensity = num.lerp(0.08, 0.35, daylight),
      exposure = 1.0,
      reflection_intensity = REFLECTION,
    })

    -- The discs hang on the sky around the camera.
    local cam = self.camera
    if self.sun_disc then
      transform.set_position(self.sun_disc, vec3.new(
        cam[1] + sx * DISC_DISTANCE, cam[2] + sy * DISC_DISTANCE, cam[3] + sz * DISC_DISTANCE))
    end
    if self.moon_disc then
      transform.set_position(self.moon_disc, vec3.new(
        cam[1] - sx * DISC_DISTANCE, cam[2] - sy * DISC_DISTANCE, cam[3] - sz * DISC_DISTANCE))
    end
  end,
})
