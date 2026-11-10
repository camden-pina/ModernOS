extern int dynamic_leaf_value(void);
int dynamic_middle_constructor;
__attribute__((constructor)) static void initialize(void) {
    dynamic_middle_constructor = dynamic_leaf_value();
}
int dynamic_middle_value(void) { return dynamic_leaf_value() + 3; }
