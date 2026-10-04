package com.overlay;

import net.fabricmc.api.ClientModInitializer;
import net.minecraft.client.MinecraftClient;

import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Отправляет координаты и углы взгляда игрока во внешний C++ оверлей через Named Pipe.
 * Пакет (72 байта, little-endian): координаты игрока, данные PlayerNew
 * и рассчитанные углы наведения.
 * Должен совпадать со struct PlayerCoords в cpp_overlay/main.cpp.
 */
public class CoordsClientMod implements ClientModInitializer {
    private static final String PIPE_PATH = "\\\\.\\pipe\\mc_coords";
    private static final int PACKET_SIZE = 72;

    @Override
    public void onInitializeClient() {
        System.out.println("[CoordsOverlay] Mod initialized for Minecraft 1.21.4!");

        Thread thread = new Thread(() -> {
            FileOutputStream pipe = null;

            while (true) {
                try {
                    Thread.sleep(50); // 20 обновлений в секунду

                    MinecraftClient client = MinecraftClient.getInstance();
                    if (client == null || client.player == null || client.world == null) {
                        continue;
                    }

                    if (pipe == null) {
                        try {
                            pipe = new FileOutputStream(PIPE_PATH);
                            System.out.println("[CoordsOverlay] Connected to C++ overlay Named Pipe!");
                        } catch (IOException e) {
                            continue; // оверлей ещё не запущен
                        }
                    }

                    ByteBuffer buf = ByteBuffer.allocate(PACKET_SIZE).order(ByteOrder.LITTLE_ENDIAN);
                    buf.putDouble(client.player.getX());
                    buf.putDouble(client.player.getY());
                    buf.putDouble(client.player.getZ());
                    buf.putFloat(client.player.getYaw());
                    buf.putFloat(client.player.getPitch());

                    PlayerNew.Snapshot target = PlayerNew.getSnapshot();
                    buf.putInt(target.hasTarget() ? 1 : 0);
                    buf.putInt(target.aiming() ? 1 : 0);
                    buf.putDouble(target.tx());
                    buf.putDouble(target.ty());
                    buf.putDouble(target.tz());
                    buf.putFloat(target.aimYaw());
                    buf.putFloat(target.aimPitch());

                    pipe.write(buf.array());
                    pipe.flush();

                } catch (IOException e) {
                    try {
                        if (pipe != null) pipe.close();
                    } catch (IOException ignored) {}
                    pipe = null;
                } catch (InterruptedException e) {
                    break;
                } catch (Exception e) {
                    // временные ошибки игнорируем
                }
            }
        }, "CoordsOverlay-PipeThread");

        thread.setDaemon(true);
        thread.start();
    }
}
