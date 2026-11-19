    a = 10
    b = 3
    f = 2.5
    d = -1.5
    t1 = a > b
    t2 = b == 0
    t3 = ! t2
    t4 = t1 && t3
    ok = t4
    t5 = a % b
    t6 = - a
    t7 = t6 * 2
    t8 = t5 + t7
    c = t8
    t9 = a + 5
    a = t9
    t10 = b * 2
    b = t10
    t11 = c - 1
    c = t11
    t12 = c + 1
    c = t12
    print "Hello, World!\n"
    print "a="
    print a
    print " b="
    print b
    print " f="
    print f
    print "\n"
    t13 = a > b
    ifFalse t13 goto L1
    print "a bigger\n"
    goto L2
L1:
    t14 = a == b
    ifFalse t14 goto L3
    print "equal\n"
    goto L4
L3:
    print "b bigger\n"
L4:
L2:
    i = 0
L5:
    t15 = i < 3
    ifFalse t15 goto L6
    print i
    print " "
    t16 = i + 1
    i = t16
    goto L5
L6:
    i_1 = 0
L7:
    t17 = i_1 < 3
    ifFalse t17 goto L8
    t18 = a + i_1
    a = t18
    t19 = i_1 + 1
    i_1 = t19
    goto L7
L8:
    ifFalse ok goto L9
    print "ok "
    print c
    print "%\n"
L9:
    return 0
