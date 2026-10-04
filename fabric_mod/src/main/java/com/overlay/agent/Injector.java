package com.overlay.agent;

/**
 * Запускается из MCOverlay.exe:  java -cp mcagent.jar com.overlay.agent.Injector <pid> <agent.jar>
 * Нужен JDK (модуль jdk.attach). Через рефлексию, чтобы не зависеть от модулей при компиляции.
 */
public final class Injector {
    public static void main(String[] a) {
        if (a.length < 2) { System.out.println("usage: Injector <pid> <agent.jar>"); System.exit(2); }
        try {
            Class<?> vmc = Class.forName("com.sun.tools.attach.VirtualMachine");
            Object vm = vmc.getMethod("attach", String.class).invoke(null, a[0]);
            try {
                vmc.getMethod("loadAgent", String.class, String.class).invoke(vm, a[1], "");
            } finally {
                vmc.getMethod("detach").invoke(vm);
            }
            System.out.println("OK");
        } catch (ClassNotFoundException e) {
            System.out.println("Нужен JDK с модулем jdk.attach (укажите JAVA_HOME или [agent] java= в MCOverlay.ini)");
            System.exit(1);
        } catch (Throwable t) {
            Throwable c = t.getCause() != null ? t.getCause() : t;
            System.out.println(c.getClass().getSimpleName() + ": " + c.getMessage());
            System.exit(1);
        }
    }
}
