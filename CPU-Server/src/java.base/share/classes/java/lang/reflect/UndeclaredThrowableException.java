/*
 * Copyright (c) 1999, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package java.lang.reflect;

import java.io.IOException;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.io.ObjectStreamField;
import jdk.internal.access.SharedSecrets;

/**
 * Thrown by a method invocation on a proxy instance if its invocation
 * handler's {@link InvocationHandler#invoke invoke} method throws a
 * checked exception (a {@code Throwable} that is not assignable
 * to {@code RuntimeException} or {@code Error}) that
 * is not assignable to any of the exception types declared in the
 * {@code throws} clause of the method that was invoked on the
 * proxy instance and dispatched to the invocation handler.
 *
 * <p>An {@code UndeclaredThrowableException} instance contains
 * the undeclared checked exception that was thrown by the invocation
 * handler, and it can be retrieved with the
 * {@code getUndeclaredThrowable()} method.
 * {@code UndeclaredThrowableException} extends
 * {@code RuntimeException}, so it is an unchecked exception
 * that wraps a checked exception.
 *
 * <p>As of release 1.4, this exception has been retrofitted to
 * conform to the general purpose exception-chaining mechanism.  The
 * "undeclared checked exception that was thrown by the invocation
 * handler" that may be provided at construction time and accessed via
 * the {@link #getUndeclaredThrowable()} method is now known as the
 * <i>cause</i>, and may be accessed via the {@link
 * Throwable#getCause()} method, as well as the aforementioned "legacy
 * method."
 *
 * @author      Peter Jones
 * @see         InvocationHandler
 * @since       1.3
 */
public class UndeclaredThrowableException extends RuntimeException {
    static final long serialVersionUID = 330127114055056639L;

    /**
     * Constructs an {@code UndeclaredThrowableException} with the
     * specified {@code Throwable}.
     *
     * @param   undeclaredThrowable the undeclared checked exception
     *          that was thrown
     */
    public UndeclaredThrowableException(Throwable undeclaredThrowable) {
        super(null, undeclaredThrowable);  // Disallow initCause
    }

    /**
     * Constructs an {@code UndeclaredThrowableException} with the
     * specified {@code Throwable} and a detail message.
     *
     * @param   undeclaredThrowable the undeclared checked exception
     *          that was thrown
     * @param   s the detail message
     */
    public UndeclaredThrowableException(Throwable undeclaredThrowable,
                                        String s)
    {
        super(s, undeclaredThrowable);  // Disallow initCause
    }

    /**
     * Returns the {@code Throwable} instance wrapped in this
     * {@code UndeclaredThrowableException}, which may be {@code null}.
     *
     * <p>This method predates the general-purpose exception chaining facility.
     * The {@link Throwable#getCause()} method is now the preferred means of
     * obtaining this information.
     *
     * @return the undeclared checked exception that was thrown
     */
    public Throwable getUndeclaredThrowable() {
        return super.getCause();
    }

    /**
     * Serializable fields for UndeclaredThrowableException.
     *
     * @serialField undeclaredThrowable Throwable
     */
    private static final ObjectStreamField[] serialPersistentFields = {
        new ObjectStreamField("undeclaredThrowable", Throwable.class)
    };

    /*
     * Reconstitutes the UndeclaredThrowableException instance from a stream
     * and initialize the cause properly when deserializing from an older
     * version.
     */
    private void readObject(ObjectInputStream s) throws IOException, ClassNotFoundException {
        ObjectInputStream.GetField fields = s.readFields();
        Throwable exception = (Throwable) fields.get("undeclaredThrowable", null);
        if (exception != null) {
            SharedSecrets.getJavaLangAccess().setCause(this, exception);
        }
    }

    /*
     * To maintain compatibility with older implementation, write a serial
     * "ex" field with the cause as the value.
     */
    private void writeObject(ObjectOutputStream out) throws IOException {
        ObjectOutputStream.PutField fields = out.putFields();
        fields.put("undeclaredThrowable", super.getCause());
        out.writeFields();
    }
}
