/*
 *  Copyright (c) 2011, Oracle and/or its affiliates. All rights reserved.
 *  Copyright (c) 2021, 2021 Logical Clocks AB and/or its affiliates.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License, version 2.0,
 *  as published by the Free Software Foundation.
 *
 *  This program is also distributed with certain software (including
 *  but not limited to OpenSSL) that is licensed under separate terms,
 *  as designated in a particular file or component or in included license
 *  documentation.  The authors of MySQL hereby grant you an additional
 *  permission to link the program and your derivative works with the
 *  separately licensed software that they have included with MySQL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License, version 2.0, for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

package testsuite.clusterj;

import java.util.ArrayList;
import java.util.List;

import testsuite.clusterj.model.LongvarcharPK;

public class LongvarcharPKTest extends AbstractClusterJTest {

    protected int NUMBER_OF_INSTANCES = 15;
    protected List<LongvarcharPK> instances = new ArrayList<LongvarcharPK>();

    @Override
    public void localSetUp() {
        createSessionFactory();
        session = sessionFactory.getSession();
        tx = session.currentTransaction();
        try {
            tx.begin();
            session.deletePersistentAll(LongvarcharPK.class);
            tx.commit();
        } catch (Throwable t) {
            // ignore errors while deleting
        }
        createInstances();
        addTearDownClasses(LongvarcharPK.class);
    }

    public void test() {
        insert();
        find();
        update();
        delete();
        failOnError();
    }

    /** Insert all instances.
     */
    protected void insert() {
        session.makePersistentAll(instances);
    }

    /** Find all instances.
     */
    protected void find() {
        for (int i = 0; i < NUMBER_OF_INSTANCES; ++i) {
            String key = getPK(i);
            LongvarcharPK result = session.find(LongvarcharPK.class, key);
            verify(result, i, false);
        }
    }

    /** Blind update every fourth instance.
     */
    protected void update() {
        // update the instances
        for (int i = 0; i < NUMBER_OF_INSTANCES; ++i) {
            if (0 == i % 4) {
                LongvarcharPK instance = createInstance(i);
                instance.setName(getValue(NUMBER_OF_INSTANCES - i));
                session.updatePersistent(instance);
                verify(instance, i, true);
            }
        }
        // verify the updated instances
        for (int i = 0; i < NUMBER_OF_INSTANCES; ++i) {
            if (0 == i % 4) {
                String key = getPK(i);
                LongvarcharPK instance = session.find(LongvarcharPK.class, key);
                verify(instance, i, true);
            }
        }
    }

    /** Blind delete every fifth instance.
     */
    protected void delete() {
        // delete the instances
        for (int i = 0; i < NUMBER_OF_INSTANCES; ++i) {
            if (0 == i % 5) {
                LongvarcharPK instance = createInstance(i);
                session.deletePersistent(instance);
            }
        }
        // verify they have been deleted
        for (int i = 0; i < NUMBER_OF_INSTANCES; ++i) {
            if (0 == i % 5) {
                String key = getPK(i);
                LongvarcharPK instance = session.find(LongvarcharPK.class, key);
                errorIfNotEqual("Failed to delete instance: " + i, null, instance);
            }
        }
    }

    /** The strategy for instances is for the "instance number" to create 
     * the keys by creating a byte[] with the encoded number.
     */
    protected void createInstances() {
        for (int i = 0; i < NUMBER_OF_INSTANCES; ++i) {
            LongvarcharPK instance = createInstance(i);
            if (getDebug()) System.out.println(toString(instance));
            instances.add(instance);
        }
    }

    /** Create an instance of LongvarcharPK.
     * @param index the index to use to generate data
     * @return the instance
     */
    protected LongvarcharPK createInstance(int index) {
        LongvarcharPK instance = session.newInstance(LongvarcharPK.class);
        instance.setId(getPK(index));
        instance.setNumber(index);
        instance.setName(getValue(index));
        return instance;
    }

    protected String toString(LongvarcharPK instance) {
        StringBuffer result = new StringBuffer();
        result.append("LongvarcharPK[");
        result.append(instance.getId());
        result.append("]: ");
        result.append(instance.getNumber());
        result.append(", \"");
        result.append(instance.getName());
        result.append("\".");
        return result.toString();
    }

    protected String getPK(int index) {
        return "PK " + index;
    }

    protected String getValue(int index) {
        return "Value " + index;
    }

    protected void verify(LongvarcharPK instance, int index, boolean updated) {
        errorIfNotEqual("id failed", getPK(index), instance.getId());
        errorIfNotEqual("number failed", index, instance.getNumber());
        if (updated) {
            errorIfNotEqual("Value failed", getValue(NUMBER_OF_INSTANCES - index), instance.getName());
        } else {
            errorIfNotEqual("Value failed", getValue(index), instance.getName());
        }
    }

    private String toString(byte[] id) {
        StringBuilder builder = new StringBuilder();
        for (int i = 0; i < id.length; ++i) {
            builder.append(String.valueOf(id[i]));
            builder.append('-');
        }
        return builder.toString();
    }
}
