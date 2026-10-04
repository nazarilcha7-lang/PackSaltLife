package com.overlay;

import net.minecraft.client.MinecraftClient;
import net.minecraft.client.network.AbstractClientPlayerEntity;
import net.minecraft.client.network.ClientPlayerEntity;
import net.minecraft.client.world.ClientWorld;
import net.minecraft.util.math.MathHelper;
import net.minecraft.util.math.Vec3d;

/**
 * Находит ближайшего игрока, считает углы (yaw/pitch), на которые нужно
 * повернуть камеру, и передаёт эту информацию в C++ HUD. Вызывается каждый кадр.
 */
public final class PlayerNew {

    // ===================== НАСТРОЙКИ =====================
    /** Максимальная дистанция поиска цели (блоков). */
    public static final double MAX_DISTANCE = 64.0;
    /** Точка прицеливания по высоте цели: 0 = ноги, 1 = макушка. */
    public static final double AIM_HEIGHT_RATIO = 0.75;
    /** Целиться только в тех, кого видно (без стен между вами). */
    public static final boolean REQUIRE_LINE_OF_SIGHT = false;
    // =====================================================

    /** Данные для информационного оверлея (читаются из другого потока). */
    public record Snapshot(boolean hasTarget, boolean aiming,
                           double tx, double ty, double tz,
                           float aimYaw, float aimPitch) {
        public static final Snapshot EMPTY = new Snapshot(false, false, 0, 0, 0, 0f, 0f);
    }

    private static volatile Snapshot snapshot = Snapshot.EMPTY;

    private PlayerNew() {}

    public static Snapshot getSnapshot() {
        return snapshot;
    }

    // ---------- Формулы ----------

    /**
     * Yaw в Minecraft: 0 = на +Z (юг), растёт по часовой стрелке (90 = на -X, запад).
     * Вектор взгляда: (-sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch)).
     * Отсюда: yaw = atan2(-dx, dz); pitch = -atan2(dy, sqrt(dx^2 + dz^2)).
     */
    public static float calcYaw(double dx, double dz) {
        return (float) Math.toDegrees(Math.atan2(-dx, dz));
    }

    public static float calcPitch(double dx, double dy, double dz) {
        return (float) -Math.toDegrees(Math.atan2(dy, Math.sqrt(dx * dx + dz * dz)));
    }

    // ---------- Основной цикл ----------

    public static void onFrame(MinecraftClient client) {
        try {
            update(client);
        } catch (Throwable t) {
            snapshot = Snapshot.EMPTY; // не роняем игру из-за ошибки в моде
        }
    }

    private static void update(MinecraftClient client) {
        ClientPlayerEntity self = client.player;
        ClientWorld world = client.world;
        if (self == null || world == null) {
            snapshot = Snapshot.EMPTY;
            return;
        }

        AbstractClientPlayerEntity target = findNearest(world, self);
        if (target == null) {
            snapshot = Snapshot.EMPTY;
            return;
        }

        float tickDelta = client.getRenderTickCounter().getTickDelta(false);
        Vec3d eye = self.getCameraPosVec(tickDelta);
        Vec3d aimPoint = target.getLerpedPos(tickDelta)
                .add(0.0, target.getHeight() * AIM_HEIGHT_RATIO, 0.0);

        double dx = aimPoint.x - eye.x;
        double dy = aimPoint.y - eye.y;
        double dz = aimPoint.z - eye.z;

        float aimYaw = calcYaw(dx, dz);
        float aimPitch = MathHelper.clamp(calcPitch(dx, dy, dz), -90.0f, 90.0f);

        // PlayerNew работает только как информационный модуль: камера Minecraft
        // не изменяется, а рассчитанные углы передаются в HUD C++.
        snapshot = new Snapshot(true, false, aimPoint.x, aimPoint.y, aimPoint.z, aimYaw, aimPitch);
    }

    private static AbstractClientPlayerEntity findNearest(ClientWorld world, ClientPlayerEntity self) {
        AbstractClientPlayerEntity best = null;
        double bestDist2 = MAX_DISTANCE * MAX_DISTANCE;
        for (AbstractClientPlayerEntity p : world.getPlayers()) {
            if (p == self || p.isSpectator() || !p.isAlive()) continue;
            double d2 = self.squaredDistanceTo(p);
            if (d2 >= bestDist2) continue;
            if (REQUIRE_LINE_OF_SIGHT && !self.canSee(p)) continue;
            bestDist2 = d2;
            best = p;
        }
        return best;
    }
}
