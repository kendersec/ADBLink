/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import com.sun.javadoc.*;
import com.sun.tools.doclets.*;
import org.clearsilver.HDF;
import org.clearsilver.CS;
import java.util.*;
import java.io.*;

public class PackageInfo extends DocInfo implements ContainerInfo
{
    public static final Comparator<PackageInfo> comparator = new Comparator<PackageInfo>() {
        public int compare(PackageInfo a, PackageInfo b) {
            return a.name().compareTo(b.name());
        }
    };

    public PackageInfo(PackageDoc pkg, String name, SourcePositionInfo position)
    {
        super(pkg.getRawCommentText(), position);
        mName = name;

        if (pkg == null) {
            throw new RuntimeException("pkg is null");
        }
        mPackage = pkg;
    }

    public String htmlPage()
    {
        String s = mName;
        s = s.replace('.', '/');
        s += "/package-summary.html";
        s = DroidDoc.javadocDir + s;
        return s;
    }

    public String fullDescriptionHtmlPage() {
        String s = mName;
        s = s.replace('.', '/');
        s += "/package-descr.html";
        s = DroidDoc.javadocDir + s;
        return s;
    }

    @Override
    public ContainerInfo parent()
    {
        return null;
    }

    @Override
    public boolean isHidden()
    {
        return comment().isHidden();
    }

    public boolean checkLevel() {
        // TODO should return false if all classes are hidden but the package isn't.
        // We don't have this so I'm not doing it now.
        return !isHidden();
    }

    public String name()
    {
        return mName;
    }

    public String qualifiedName()
    {
        return mName;
    }

    public TagInfo[] inlineTags()
    {
        return comment().tags();
    }

    public TagInfo[] firstSentenceTags()
    {
        return comment().briefTags();
    }

    public static ClassInfo[] filterHidden(ClassInfo[] classes)
    {
        ArrayList<ClassInfo> out = new ArrayList<ClassInfo>();

        for (ClassInfo cl: classes) {
            if (!cl.isHidden()) {
                out.add(cl);
            }
        }

        return out.toArray(new ClassInfo[0]);
    }

    public void makeLink(HDF data, String base)
    {
        if (checkLevel()) {
            data.setValue(base + ".link", htmlPage());
        }
        data.setValue(base + ".name", name());
        data.setValue(base + ".since", getSince());
    }

    public void makeClassLinkListHDF(HDF data, String base)
    {
        makeLink(data, base);
        ClassInfo.makeLinkListHDF(data, base + ".interfaces", interfaces());
        ClassInfo.makeLinkListHDF(data, base + ".classes", ordinaryClasses());
        ClassInfo.makeLinkListHDF(data, base + ".enums", enums());
        ClassInfo.makeLinkListHDF(data, base + ".exceptions", exceptions());
        ClassInfo.makeLinkListHDF(data, base + ".errors", errors());
        data.setValue(base + ".since", getSince());
    }

    public ClassInfo[] interfaces()
    {
        if (mInterfaces == null) {
            mInterfaces = ClassInfo.sortByName(filterHidden(Converter.convertClasses(
                            mPackage.interfaces())));
        }
        return mInterfaces;
    }

    public ClassInfo[] ordinaryClasses()
    {
        if (mOrdinaryClasses == null) {
            mOrdinaryClasses = ClassInfo.sortByName(filterHidden(Converter.convertClasses(
                            mPackage.ordinaryClasses())));
        }
        return mOrdinaryClasses;
    }

    public ClassInfo[] enums()
    {
        if (mEnums == null) {
            mEnums = ClassInfo.sortByName(filterHidden(Converter.convertClasses(mPackage.enums())));
        }
        return mEnums;
    }

    public ClassInfo[] exceptions()
    {
        if (mExceptions == null) {
            mExceptions = ClassInfo.sortByName(filterHidden(Converter.convertClasses(
                        mPackage.exceptions())));
        }
        return mExceptions;
    }

    public ClassInfo[] errors()
    {
        if (mErrors == null) {
            mErrors = ClassInfo.sortByName(filterHidden(Converter.convertClasses(
                        mPackage.errors())));
        }
        return mErrors;
    }

    // in hashed containers, treat the name as the key
    @Override
    public int hashCode() {
        return mName.hashCode();
    }

    private String mName;
    private PackageDoc mPackage;
    private ClassInfo[] mInterfaces;
    private ClassInfo[] mOrdinaryClasses;
    private ClassInfo[] mEnums;
    private ClassInfo[] mExceptions;
    private ClassInfo[] mErrors;
}

