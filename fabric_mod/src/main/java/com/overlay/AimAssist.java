package com.overlay;

import net.minecraft.client.MinecraftClient;
import net.minecraft.client.network.ClientPlayerEntity;
import net.minecraft.client.world.ClientWorld;
import net.minecraft.entity.Entity;
import net.minecraft.entity.LivingEntity;
import net.minecraft.entity.mob.HostileEntity;
import net.minecraft.entity.passive.TameableEntity;
import net.minecraft.entity.player.PlayerEntity;
import net.minecraft.util.math.MathHelper;
import net.minecraft.util.math.Vec3d;

import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;
import java.util.concurrent.ThreadLocalRandom;

/**
 * Плавная наводка. ТОЛЬКО одиночный мир (isInSingleplayer).
 * Настройки: %APPDATA%\MCOverlay\aim.properties (пишет MCOverlay.exe).
 * Вызывается каждый кадр из MinecraftClientMixin.
 */
public final class AimAssist {

    private record Settings(boolean enabled, double range, double fov, double speed,
                            double slowRadius, double spread, double jitter, double flickPerSec,
                            boolean hostile, boolean passive, boolean ignoreTamed, boolean requireLos,
                            boolean players) {
        static final Settings OFF = new Settings(false, 12, 60, 240, 25, 0.6, 0.15, 0.4,
                true, false, true, true, false);
    }

    private static final Path CONFIG_PATH = resolveConfigPath();
    private static volatile Settings settings = Settings.OFF;
    private static long lastCheckMs = 0, lastModified = -1;

    // состояние наводки
    private static LivingEntity target;
    private static long lastNanos = 0;

    private static double offX, offZ, ratio = 0.7, tgtOffX, tgtOffZ, tgtRatio = 0.7;
    private static long nextOffsetMs = 0;
    private static double speedMul = 1.0, speedTarget = 1.0;
    private static long nextSpeedMs = 0;
    private static long flickUntilMs = 0;
    private static double overshootYaw = 0, overshootPitch = 0;
    private static double jitterYaw = 0, jitterPitch = 0;
    private static double arcPhase = 0;

    private AimAssist() {}

    private static Path resolveConfigPath() {
        String base = System.getenv("APPDATA");
        if (base == null || base.isEmpty()) base = System.getProperty("user.home");
        return Path.of(base, "MCOverlay", "aim.properties");
    }

    public static void onFrame(MinecraftClient client) {
        try {
            reloadIfChanged();
            run(client);
        } catch (Throwable t) {
            target = null;
            t.printStackTrace();
        }
    }

    private static void run(MinecraftClient client) {
        Settings cfg = settings;
        ClientPlayerEntity player = client.player;
        ClientWorld world = client.world;

        long nowNanos = System.nanoTime();
        double dt = lastNanos == 0 ? 0.016 : Math.min(0.1, (nowNanos - lastNanos) / 1e9);
        lastNanos = nowNanos;
        long nowMs = System.currentTimeMillis();

        if (!cfg.enabled() || player == null || world == null
                || !client.isInSingleplayer()
                || client.currentScreen != null || player.isSpectator()) {
            target = null;
            return;
        }

        float yaw = player.getYaw(), pitch = player.getPitch();

        float tickDelta = client.getRenderTickCounter().getTickDelta(false);
        Vec3d eye = player.getCameraPosVec(tickDelta);

        // Выбор / удержание цели
        if (target == null || !isValid(cfg, player, target)
                || player.distanceTo(target) > cfg.range()) {
            LivingEntity old = target;
            target = pickTarget(cfg, world, player, eye, yaw, pitch, tickDelta);
            if (target != old) newTarget();
        }
        if (target == null) return;

        // Случайная точка в хитбоксе, смещение меняется плавно
        ThreadLocalRandom r = ThreadLocalRandom.current();
        if (nowMs >= nextOffsetMs) {
            double w = target.getWidth();
            tgtOffX = (r.nextDouble() * 2 - 1) * w * 0.35 * cfg.spread();
            tgtOffZ = (r.nextDouble() * 2 - 1) * w * 0.35 * cfg.spread();
            tgtRatio = 0.7 + (r.nextDouble() * 2 - 1) * 0.3 * cfg.spread();
            nextOffsetMs = nowMs + r.nextLong(250, 700);
        }
        double k = 1 - Math.exp(-dt * 6);
        offX += (tgtOffX - offX) * k;
        offZ += (tgtOffZ - offZ) * k;
        ratio += (tgtRatio - ratio) * k;

        Vec3d aim = target.getLerpedPos(tickDelta)
                .add(offX, target.getHeight() * ratio, offZ);
        double dx = aim.x - eye.x, dy = aim.y - eye.y, dz = aim.z - eye.z;
        float wantYaw = PlayerNew.calcYaw(dx, dz);
        float wantPitch = MathHelper.clamp(PlayerNew.calcPitch(dx, dy, dz), -90f, 90f);

        double baseYaw = MathHelper.wrapDegrees(wantYaw - yaw);
        double basePitch = wantPitch - pitch;
        if (Math.hypot(baseYaw, basePitch) > cfg.fov() * 1.5) { target = null; return; }

        // Переменная скорость
        if (nowMs >= nextSpeedMs) {
            speedTarget = 0.65 + r.nextDouble() * 0.7;
            nextSpeedMs = nowMs + r.nextLong(150, 400);
        }
        speedMul += (speedTarget - speedMul) * (1 - Math.exp(-dt * 8));

        // Резкий рывок с перелётом
        if (nowMs >= flickUntilMs && r.nextDouble() < cfg.flickPerSec() * dt) {
            flickUntilMs = nowMs + r.nextLong(80, 150);
            overshootYaw = (r.nextBoolean() ? 1 : -1) * (1 + r.nextDouble() * 3);
            overshootPitch = (r.nextBoolean() ? 1 : -1) * (0.5 + r.nextDouble() * 1.5);
        }
        boolean flicking = nowMs < flickUntilMs;
        double decay = Math.exp(-dt * 5);
        overshootYaw *= decay;
        overshootPitch *= decay;

        // Дрожь
        double a = Math.exp(-dt * 10);
        jitterYaw = jitterYaw * a + (r.nextDouble() * 2 - 1) * cfg.jitter() * (1 - a);
        jitterPitch = jitterPitch * a + (r.nextDouble() * 2 - 1) * cfg.jitter() * (1 - a);

        double ey = baseYaw + overshootYaw + jitterYaw;
        double ep = basePitch + overshootPitch + jitterPitch;
        double dist = Math.hypot(ey, ep);
        if (dist < 0.2) return; // мёртвая зона

        // Замедление у цели + скорость
        double ease = MathHelper.clamp(dist / cfg.slowRadius(), 0.12, 1.0);
        double speed = cfg.speed() * speedMul * ease * (flicking ? 3.0 : 1.0);
        double step = Math.min(dist, speed * dt);

        double ux = ey / dist, uy = ep / dist;
        // Дуга: боковая составляющая, затухает у цели
        arcPhase += dt * 2.2;
        double lateral = step * Math.sin(arcPhase) * 0.25 * Math.min(1.0, dist / 10.0);
        double dYaw = ux * step + (-uy) * lateral;
        double dPitch = uy * step + ux * lateral;

        // Поворот как от реальной мыши (0.15 градуса на единицу курсора)
        player.changeLookDirection(dYaw / 0.15, dPitch / 0.15);
    }

    private static void newTarget() {
        offX = offZ = 0;
        nextOffsetMs = 0;
        overshootYaw = overshootPitch = 0;
    }

    private static LivingEntity pickTarget(Settings cfg, ClientWorld world, ClientPlayerEntity player,
                                           Vec3d eye, float yaw, float pitch, float tickDelta) {
        LivingEntity best = null;
        double bestAngle = cfg.fov();
        for (Entity e : world.getEntities()) {
            if (!(e instanceof LivingEntity le) || !isValid(cfg, player, le)) continue;
            if (player.distanceTo(le) > cfg.range()) continue;
            Vec3d p = le.getLerpedPos(tickDelta).add(0, le.getHeight() * 0.7, 0);
            double dx = p.x - eye.x, dy = p.y - eye.y, dz = p.z - eye.z;
            double ay = MathHelper.wrapDegrees(PlayerNew.calcYaw(dx, dz) - yaw);
            double ap = PlayerNew.calcPitch(dx, dy, dz) - pitch;
            double ang = Math.hypot(ay, ap);
            if (ang < bestAngle) { bestAngle = ang; best = le; }
        }
        return best;
    }

    private static boolean isValid(Settings cfg, ClientPlayerEntity self, LivingEntity e) {
        if (e == self || !e.isAlive()) return false;
        if (cfg.requireLos() && !self.canSee(e)) return false;
        if (e instanceof PlayerEntity p) return cfg.players() && !p.isSpectator();
        if (e instanceof TameableEntity t && t.isTamed() && cfg.ignoreTamed()) return false;
        if (e instanceof HostileEntity) return cfg.hostile();
        return cfg.passive();
    }

    // ---------- Чтение настроек ----------

    private static void reloadIfChanged() {
        long now = System.currentTimeMillis();
        if (now - lastCheckMs < 1000) return;
        lastCheckMs = now;
        try {
            if (!Files.exists(CONFIG_PATH)) { settings = Settings.OFF; lastModified = -1; return; }
            long mod = Files.getLastModifiedTime(CONFIG_PATH).toMillis();
            if (mod == lastModified) return;
            lastModified = mod;
            Properties p = new Properties();
            try (InputStream in = Files.newInputStream(CONFIG_PATH)) { p.load(in); }
            settings = new Settings(
                    bool(p, "enabled", false),
                    clamp(dbl(p, "range", 12), 2, 32),
                    clamp(dbl(p, "fov", 60), 5, 180),
                    clamp(dbl(p, "speed", 240), 30, 900),
                    clamp(dbl(p, "slowRadius", 25), 5, 90),
                    clamp(dbl(p, "spread", 0.6), 0, 1),
                    clamp(dbl(p, "jitter", 0.15), 0, 1),
                    clamp(dbl(p, "flick", 0.4), 0, 3),
                    bool(p, "hostile", true),
                    bool(p, "passive", false),
                    bool(p, "ignoreTamed", true),
                    bool(p, "requireLos", true),
                    bool(p, "players", false));
        } catch (Exception ignored) {
        }
    }

    private static boolean bool(Properties p, String k, boolean d) {
        String v = p.getProperty(k);
        return v == null ? d : v.trim().equals("1") || v.trim().equalsIgnoreCase("true");
    }

    private static double dbl(Properties p, String k, double d) {
        try { return Double.parseDouble(p.getProperty(k, String.valueOf(d)).trim()); }
        catch (NumberFormatException e) { return d; }
    }

    private static double clamp(double v, double lo, double hi) { return Math.max(lo, Math.min(hi, v)); }
}